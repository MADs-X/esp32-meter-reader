#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/ip6.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "sqlite3.h"
#include "meter_protocol.h"
#include "meter_db.h"
#include "meter_scheduler.h"
#include "wifi_manager.h"
#include "wireguard_manager.h"
#include "db_migration.h"
#include "udp_handlers.h"
static const char *TAG = "SLIP_GATEWAY";
static volatile bool cc1312_prefix_synced = false;
// --- Donanım ve Bellek Tanımlamaları ---
#define CC1312_UART_NUM  UART_NUM_1
#define CC1312_TX_PIN    43 
#define CC1312_RX_PIN    44 
#define CC1312_RESET_PIN 40

#define UART_RX_BUF_SIZE (4096)
#define UART_TX_BUF_SIZE (4096)
#define SLIP_MTU         (1280)

// --- SLIP Protokol Sabitleri ---
#define SLIP_END         0xC0
#define SLIP_ESC         0xDB
#define SLIP_ESC_END     0xDC
#define SLIP_ESC_ESC     0xDD
extern sqlite3* get_db_handle(void);

// --- Global Değişkenler ---
static QueueHandle_t uart_queue;
struct netif my_slip_netif; 
static SemaphoreHandle_t slip_tx_mutex = NULL;
static uint8_t slip_tx_buffer[SLIP_MTU * 2 + 2];
QueueHandle_t sensor_queue = NULL;
// ==========================================
// 1. FAZ: UART DONANIM BAŞLATMA
// ==========================================
void init_cc1312_uart(void) {
    const uart_config_t uart_config = {
        .baud_rate = 921600,          
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, 
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(CC1312_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CC1312_UART_NUM, CC1312_TX_PIN, CC1312_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(gpio_set_pull_mode(CC1312_RX_PIN, GPIO_PULLUP_ONLY)); 
    
    ESP_ERROR_CHECK(uart_driver_install(CC1312_UART_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 40, &uart_queue, 0));
    ESP_LOGI(TAG, "UART1 Event-Driven modda baslatildi");
}

// ==========================================
// 2. FAZ: TX VE NETIF AYARLARI
// ==========================================
#if LWIP_IPV4
err_t dummy_output_ipv4(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr) {
    return ERR_IF; // Sadece IPv6 destekliyoruz
}
#endif

err_t slip_output_ip6(struct netif *netif, struct pbuf *p, const ip6_addr_t *ipaddr) {
    if (p == NULL) return ERR_VAL;

    if (!cc1312_prefix_synced) {
        return ERR_OK; // Prefix senkronize olmadan IPv6 paketi yönlendirme
    }

    if (xSemaphoreTake(slip_tx_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint16_t tx_idx = 0;
        
        slip_tx_buffer[tx_idx++] = SLIP_END; 

        for (struct pbuf *q = p; q != NULL; q = q->next) {
            uint8_t *payload = (uint8_t *)q->payload;
            for (uint16_t i = 0; i < q->len; i++) {
                if (tx_idx >= sizeof(slip_tx_buffer) - 2) {
                    ESP_LOGE(TAG, "TX Buffer tasmasi engellendi!");
                    xSemaphoreGive(slip_tx_mutex);
                    return ERR_BUF;
                }

                uint8_t c = payload[i];
                if (c == SLIP_END) {
                    slip_tx_buffer[tx_idx++] = SLIP_ESC;
                    slip_tx_buffer[tx_idx++] = SLIP_ESC_END;
                } else if (c == SLIP_ESC) {
                    slip_tx_buffer[tx_idx++] = SLIP_ESC;
                    slip_tx_buffer[tx_idx++] = SLIP_ESC_ESC;
                } else {
                    slip_tx_buffer[tx_idx++] = c;
                }
            }
        }
        slip_tx_buffer[tx_idx++] = SLIP_END; 
        uart_write_bytes(CC1312_UART_NUM, (const char*)slip_tx_buffer, tx_idx);
        xSemaphoreGive(slip_tx_mutex);
        return ERR_OK;
    }
    
    ESP_LOGE(TAG, "TX Mutex Timeout! Paket drop edildi.");
    return ERR_TIMEOUT;
}

err_t slip_netif_init_cb(struct netif *netif) {
    netif->name[0] = 's';
    netif->name[1] = 'l';
    netif->output_ip6 = slip_output_ip6; 
    
#if LWIP_IPV4
    //netif->output = dummy_output_ipv4; 
#endif

    netif->mtu = SLIP_MTU;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, mac, 6);

    netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP | NETIF_FLAG_MLD6; 
    return ERR_OK;
}

void init_slip_netif(void) {
    ESP_LOGI(TAG, "SLIP Sanal Ag Karti (Netif) baslatiliyor...");
    
    if (slip_tx_mutex == NULL) {
        slip_tx_mutex = xSemaphoreCreateMutex();
    }
    
    LOCK_TCPIP_CORE(); 
    // IPv4 adresleri (kullanılmayacak, ama prototip gereği doldurulmalı)
    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr, 0, 0, 0, 0);
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    netif_add(&my_slip_netif, &ipaddr, &netmask, &gw, NULL, slip_netif_init_cb, tcpip_input);
    //netif_set_default(&my_slip_netif);
    netif_set_up(&my_slip_netif);
    netif_set_link_up(&my_slip_netif);
    
    // 1. Link-Local Adresi Oluştur
    netif_create_ip6_linklocal_address(&my_slip_netif, 1);
    my_slip_netif.ip6_autoconfig_enabled = 1;

    // ---- YENİ EKLENEN KISIM: STATİK GLOBAL IPv6 ADRESİ (SENSÖRÜN BEKLEDİĞİ IP) ----
    ip_addr_t my_global_ip6;
    // Sensörlerin "report addr" olarak log bastığı hedef IP adresini (fd00::ff:fe00:5) LwIP'ye öğretiyoruz.
    ipaddr_aton("fd00::ff:fe00:5", &my_global_ip6); 
    
    int8_t ip6_idx;
    // Bu IP adresini "Valid" ve "Preferred" (Geçerli ve Tercih Edilen) olarak Netif'e ekle
    err_t err = netif_add_ip6_address(&my_slip_netif, ip_2_ip6(&my_global_ip6), &ip6_idx);
    
    if (err == ERR_OK) {
        netif_ip6_addr_set_state(&my_slip_netif, ip6_idx, IP6_ADDR_PREFERRED);
        ESP_LOGI(TAG, "Statik Global IPv6 (fd00::ff:fe00:5) SLIP Netif'e eklendi.");
    } else {
        ESP_LOGE(TAG, "Statik IPv6 eklenirken hata olustu: %d", err);
    }
    // --------------------------------------------------------------------------------
    
    UNLOCK_TCPIP_CORE();
    
    ESP_LOGI(TAG, "SLIP Netif basariyla eklendi (sl0).");
}

// ==========================================
// 3. FAZ: RX (ALMA), PROTOKOL VE PAKET ÇÖZÜMLEME
// ==========================================
void send_mac_to_cc1312(void) {
    if (xSemaphoreTake(slip_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    
    uint16_t tx_idx = 0;
    slip_tx_buffer[tx_idx++] = SLIP_END; 
    slip_tx_buffer[tx_idx++] = SLIP_END; // MAC anonsu için de çift flush garantisi
    
    slip_tx_buffer[tx_idx++] = '!';
    slip_tx_buffer[tx_idx++] = 'M';
    
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    uint8_t eui64[8] = { mac[0] ^ 0x02, mac[1], mac[2], 0xFF, 0xFE, mac[3], mac[4], mac[5] };
    
    for (int i=0; i<8; i++) {
        uint8_t c = eui64[i];
        if (c == SLIP_END) { slip_tx_buffer[tx_idx++] = SLIP_ESC; slip_tx_buffer[tx_idx++] = SLIP_ESC_END; }
        else if (c == SLIP_ESC) { slip_tx_buffer[tx_idx++] = SLIP_ESC; slip_tx_buffer[tx_idx++] = SLIP_ESC_ESC; }
        else { slip_tx_buffer[tx_idx++] = c; }
    }
    slip_tx_buffer[tx_idx++] = SLIP_END;
    
    uart_write_bytes(CC1312_UART_NUM, (const char*)slip_tx_buffer, tx_idx);
    uart_wait_tx_done(CC1312_UART_NUM, pdMS_TO_TICKS(100));
    xSemaphoreGive(slip_tx_mutex);
}

void send_ipv6_prefix_to_cc1312(void) {
    if (xSemaphoreTake(slip_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    
    uint16_t tx_idx = 0;
    
    // Çift SLIP_END ile Contiki'nin RX buffer'ını kesin olarak sıfırla
    slip_tx_buffer[tx_idx++] = SLIP_END; 
    slip_tx_buffer[tx_idx++] = SLIP_END; 
    
    slip_tx_buffer[tx_idx++] = '!';
    slip_tx_buffer[tx_idx++] = 'P';
    
    uint8_t prefix[16] = {
        0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    for (int i=0; i<16; i++) {
        uint8_t c = prefix[i];
        if (c == SLIP_END) { slip_tx_buffer[tx_idx++] = SLIP_ESC; slip_tx_buffer[tx_idx++] = SLIP_ESC_END; }
        else if (c == SLIP_ESC) { slip_tx_buffer[tx_idx++] = SLIP_ESC; slip_tx_buffer[tx_idx++] = SLIP_ESC_ESC; }
        else { slip_tx_buffer[tx_idx++] = c; }
    }
    slip_tx_buffer[tx_idx++] = SLIP_END;
    
    uart_write_bytes(CC1312_UART_NUM, (const char*)slip_tx_buffer, tx_idx);
    uart_wait_tx_done(CC1312_UART_NUM, pdMS_TO_TICKS(100));
    xSemaphoreGive(slip_tx_mutex);
}


void process_packet(uint8_t *payload, uint16_t length) {
    if (length == 0) return;

    if (length == 2 && payload[0] == '?' && payload[1] == 'P') {
        // UART çarpışmasını engellemek için garanti 100ms gecikme
        vTaskDelay(pdMS_TO_TICKS(100)); 
        send_ipv6_prefix_to_cc1312();
        cc1312_prefix_synced = true;
        ESP_LOGI(TAG, "Prefix anonsu CC1312'ye gonderildi (!P).");
        return; 
    }
    
    if (length == 2 && payload[0] == '?' && payload[1] == 'M') {
        // MAC talebinde de aynı çakışmayı önlemek için gecikme eklendi
        vTaskDelay(pdMS_TO_TICKS(100)); 
        send_mac_to_cc1312();
        ESP_LOGI(TAG, "MAC adresi anonsu CC1312'ye gonderildi (!M).");
        return; 
    }

    if ((payload[0] & 0xF0) == 0x60) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);
        if (p != NULL) {
            pbuf_take(p, payload, length);
            if (my_slip_netif.input(p, &my_slip_netif) != ERR_OK) {
                pbuf_free(p); 
            }
        } else {
            ESP_LOGE(TAG, "RX RAM Tükendi! pbuf_alloc başarisiz.");
        }
    } else {
        ESP_LOGI("CC1312_LOG", "%.*s", length, (char *)payload);
    }
}

void slip_rx_task(void *arg) {
    uart_event_t event;
    
    uint8_t* temp_rx_buf = (uint8_t*) heap_caps_malloc(UART_RX_BUF_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (temp_rx_buf == NULL) {
        ESP_LOGE(TAG, "UART RX icin Heap allocation failed!");
        vTaskDelete(NULL);
    }

    uint8_t slip_rx_buffer[SLIP_MTU]; 
    uint16_t slip_rx_len = 0;
    bool is_escaped = false;
    bool is_dropping = false; 

    while (1) {
        if (xQueueReceive(uart_queue, (void *)&event, portMAX_DELAY)) {
            
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                ESP_LOGE(TAG, "UART Overflow (Type: %d)! Buffer temizleniyor.", event.type);
                uart_flush_input(CC1312_UART_NUM);
                xQueueReset(uart_queue);
                is_dropping = true; 
                slip_rx_len = 0;
                is_escaped = false;
                continue;
            }

            if (event.type == UART_DATA) {
                int read_len = uart_read_bytes(CC1312_UART_NUM, temp_rx_buf, event.size, portMAX_DELAY);
                
                for (int i = 0; i < read_len; i++) {
                    uint8_t c = temp_rx_buf[i];
                    
                    // KURAL 1: Standart SLIP Çerçevesi (0xC0)
                    if (c == SLIP_END) {
                        if (!is_dropping && slip_rx_len > 0) {
                            process_packet(slip_rx_buffer, slip_rx_len); 
                        }
                        slip_rx_len = 0;
                        is_escaped = false;
                        is_dropping = false; 
                    } 
                    // KURAL 2: ASCII Log Yakalama Hilesi
                    // Eger gelen karakter satir sonu (\n) ise ve paket IPv6 (0x60) veya SLIP komutu (?) degilse:
                    else if (c == '\n' && slip_rx_len > 0 && slip_rx_buffer[0] != 0x60 && slip_rx_buffer[0] != '?') {
                        
                        // Ekrandaki bozuk \r karakterini temizle
                        int print_len = slip_rx_len;
                        if (slip_rx_buffer[print_len - 1] == '\r') print_len--;
                        
                        //ESP_LOGI("CC1312_TXT", "%.*s", print_len, (char*)slip_rx_buffer);
                        
                        slip_rx_len = 0;
                        is_escaped = false;
                        is_dropping = false;
                    }
                    // KURAL 3: Veri Biriktirme
                    else if (!is_dropping) {
                        if (c == SLIP_ESC) {
                            is_escaped = true;
                        } 
                        else {
                            if (is_escaped) {
                                if (c == SLIP_ESC_END) c = SLIP_END;
                                else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
                                is_escaped = false;
                            }
                            
                            if (slip_rx_len < SLIP_MTU) {
                                slip_rx_buffer[slip_rx_len++] = c;
                            } else {
                                ESP_LOGE(TAG, "SLIP MTU Asimi! Frame drop ediliyor.");
                                is_dropping = true; 
                            }
                        }
                    }
                }
            }
        }
    }
    
    free(temp_rx_buf);
    vTaskDelete(NULL);
}/* silinecek bunlar
//parser
void parse_sensor_payload(const uint8_t *data, int len, 
                          const struct sockaddr_in6 *source_addr,
                          uint16_t src_port) {

    if (src_port != 8007) {
//        ESP_LOGW("PARSER", "Port %d'den gelen veri parse edilmedi (sadece 8007)", src_port);
        return;
    }

    // Paket başlık ve uzunluk kontrolü
    if (len < 9) {
        ESP_LOGW("PARSER", "Paket cok kisa (%d byte), parse edilmedi.", len);
        return;
    }
    if (data[0] != '#' || data[1] != '#') {
        ESP_LOGW("PARSER", "Gecersiz paket basligi! Beklenen: ##");
        return;
    }

    // RSSI ve Firmware
    int8_t rssi = (int8_t)data[4];
    uint32_t fw_version = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);

    char addr_str[128];
    inet_ntop(AF_INET6, &source_addr->sin6_addr, addr_str, sizeof(addr_str));

    ESP_LOGI("PARSER", "Kaynak IP: %s", addr_str);
    ESP_LOGI("PARSER", "RSSI: %d dBm", rssi);
    ESP_LOGI("PARSER", "Firmware Versiyonu: %lu", fw_version);

    // Rota (hop) bilgilerini tek satırda ve kaç hop olduğunu göster
    int start = 9;
    if (len > start + 1) {
        int hop_count = 0;
        char route_str[128] = {0};
        char hop_str[16];

        for (int i = start; i < len - 1; i += 2) {
            uint16_t node_id = (data[i] << 8) | data[i + 1];
            if (hop_count == 0) {
                snprintf(hop_str, sizeof(hop_str), "0x%04X", node_id);
            } else {
                snprintf(hop_str, sizeof(hop_str), " -> 0x%04X", node_id);
            }
            strncat(route_str, hop_str, sizeof(route_str) - strlen(route_str) - 1);
            hop_count++;
        }
        ESP_LOGI("PARSER", "Paket Rotasi: %d hop, yol: %s", hop_count, route_str);
    } else {
        ESP_LOGI("PARSER", "Paket Rotasi: Dogrudan Baglanti (0 hop)");
    }
}
//
//UDP
//
void udp_server_task(void *pvParameters)
{
    int listen_port = (int)(intptr_t)pvParameters; // dinlenen port (örneğin 8007)
    char rx_buffer[256];

    while (1) {
        struct sockaddr_in6 dest_addr = {0};
        dest_addr.sin6_family = AF_INET6;
        dest_addr.sin6_port = htons(listen_port);
        dest_addr.sin6_addr = in6addr_any;

        int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
        if (sock < 0) {
            ESP_LOGE("UDP", "Socket olusturulamadi, port %d", listen_port);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE("UDP", "Bind basarisiz, port %d", listen_port);
            close(sock);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI("UDP", "Port %d dinleniyor (IPv6)", listen_port);

        while (1) {
            struct sockaddr_storage source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                ESP_LOGE("UDP", "recvfrom hatasi, errno %d", errno);
                break;
            }

            rx_buffer[len] = 0;

            // Kaynak portu al
            uint16_t src_port = 0;
            if (source_addr.ss_family == AF_INET6) {
                src_port = ntohs(((struct sockaddr_in6 *)&source_addr)->sin6_port);
            }

            // Her gelen paket için log
            ESP_LOGW("UDP_TASK", "Veri yakalandı, kaynak port: %d, boyut: %d byte", src_port, len);
            ESP_LOG_BUFFER_HEX("UDP_TASK_HEX", rx_buffer, len);

            // Parse et (sadece 8007 portu için)
            parse_sensor_payload((const uint8_t*)rx_buffer, len,
                                 (struct sockaddr_in6 *)&source_addr,
                                 src_port);
        }

        close(sock);
        ESP_LOGE("UDP", "Soket kapatildi, yeniden aciliyor...");
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}*/
static void test_vpn_http_request(void) {
    esp_http_client_config_t config = {
        .url = "http://10.8.0.251/", 
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
    };

    ESP_LOGI("HTTP_TEST", "ESP32 tünel üzerinden istek atıyor: %s", config.url);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE("HTTP_TEST", "HTTP Client başlatılamadı.");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI("HTTP_TEST", ">>> BAŞARILI! Sunucu Yanıtı: %d <<<", status_code);
    } else {
        ESP_LOGE("HTTP_TEST", ">>> BAŞARISIZ! İstek gitmedi. Hata: %s <<<", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}
// ==========================================
// 4. FAZ: ANA UYGULAMA (APP MAIN)
// ==========================================


void app_main(void) {
    ESP_LOGI(TAG, "Endustriyel IoT Gateway Baslatiliyor...");

    // =========================================================
    // 0. NVS BAŞLATMA (Wi-Fi için Zorunlu Ön Koşul)
    // =========================================================
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. Mutex önceden oluşturulmalı ki Netif Initialize olurken hata vermesin
    if (slip_tx_mutex == NULL) {
        slip_tx_mutex = xSemaphoreCreateMutex();
    }

    // 2. LwIP ve Event Loop Baslatma
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // =========================================================
    // 2.5 WI-FI VE ZAMAN (SNTP) SENKRONİZASYONU
    // =========================================================
    ESP_LOGI(TAG, "Wi-Fi baslatiliyor...");
    

    wifi_manager_start("mavialp", "highfi2468"); 

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "Wi-Fi baglantisi basarili, IP alindi.");

    ESP_LOGI(TAG, "SNTP baslatiliyor, zaman senkronizasyonu bekleniyor...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    int retry = 0;
    const int retry_count = 15;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Zaman bekleniyor... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Sistem saati guncellendi: %s", asctime(&timeinfo));


    // 3. UART VE NETIF BAŞLATILMASI
    init_cc1312_uart();
    init_slip_netif();

    // RX Task'i dinlemeye başlıyor...
    xTaskCreatePinnedToCore(slip_rx_task, "slip_rx_task", 8192, NULL, 10, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // Task'in ayağa kalkması için ufak bir pay

    // 4. CC1312 RESET İŞLEMLERİ
    gpio_reset_pin(CC1312_RESET_PIN);
    gpio_set_direction(CC1312_RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(CC1312_RESET_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "CC1312 Donanimsal olarak resetleniyor...");
    gpio_set_level(CC1312_RESET_PIN, 0); 
    vTaskDelay(pdMS_TO_TICKS(100));           

    ESP_LOGI(TAG, "CC1312 Uyandiriliyor...");
    gpio_set_level(CC1312_RESET_PIN, 1); 

    // =========================================================
    // SADECE EKLENEN KISIM (METER READ & DB SYSTEM)
    // =========================================================
    // WireGuard'ı BAŞLAT (Saat güncel ve Wi-Fi bağlı olduğu için sorunsuz el sıkışır)
    if (wireguard_manager_start() == ESP_OK) {
        ESP_LOGI(TAG, "WireGuard is active and routed.");
    } else {
        ESP_LOGW(TAG, "WireGuard not started or failed.");
    }
    vTaskDelay(pdMS_TO_TICKS(15000)); 
    test_vpn_http_request();

    init_vfs();        
    init_database();

    /*migrations mevzusu için
    sqlite3 *db_handle = get_db_handle();
    if (db_handle) {
        ESP_LOGI(TAG, "OTA DB Migration kontrolu basliyor...");
        esp_err_t mig_err = ota_db_migration_run(db_handle);
        if (mig_err != ESP_OK) {
            ESP_LOGW(TAG, "Migration yapilamadi (Hata: %s). Cihaz mevcut veritabaniyla calismaya devam ediyor.", esp_err_to_name(mig_err));
        }
    }*/

     // Test amaçlı ota
    /*
    ESP_ERROR_CHECK(ota_fsm_init());
    
   
    ota_request_t req = {
        .target = OTA_TARGET_ESP32,
        .fw_url = "https://server.com/esp32.bin",
        .expected_sha256 = "..."
    };
    ota_fsm_trigger_update(&req);
    */
    // B. Veritabanı Yazma Kuyruğu
    sensor_queue = xQueueCreate(20, sizeof(db_write_request_t));

    // C. Arka Plan SQLite Kayıt Task'ı
    xTaskCreate(sqlite_task, "sqlite_task", 8192, NULL, 5, NULL);

    // D. Mevcut UDP Dinleyicileri (Sensör Portları)
    ESP_LOGI(TAG, "UDP Dinleyiciler baslatiliyor...");
    init_udp_handlers();

    // F. Periyodik Okuma İstek Motoru (Scheduler)
    xTaskCreate(meter_scheduler_task, "meter_sched", 8192, NULL, 7, NULL);

    // =========================================================

    // Ana Döngü
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}