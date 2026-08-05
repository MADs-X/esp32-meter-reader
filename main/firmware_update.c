#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "driver/gpio.h"
#include "driver/uart.h"

// --- Protokol ve Donanım Sabitleri ---
#define OTA_V1                  0x01
#define OTA_REQUEST             0x00
#define OTA_RESPONSE            0x01

#define OTA_CMD_DATA            0x01
#define OTA_CMD_INFO            0x02
#define OTA_CMD_CANCEL          0x03
#define OTA_SUCCESS             0x03
#define OTA_ERR_VER             0x04

#define UDP_LISTEN_PORT         6000
#define FW_FILE_PATH            "/littlefs/firmware_update.bin"
#define LZ77_OUT_PATH           "/littlefs/firmware_uncompressed.bin"

// Donanım Pin ve Port Tanımları
#define CC1312_UART_NUM         UART_NUM_1
#define CC1312_RESET_PIN        GPIO_NUM_40
#define CC1312_BL_EN_PIN        GPIO_NUM_41

// TI SBL Protokol ve Bellek Sabitleri
#define SBL_ACK                 0x55
#define SBL_NACK                0x33
#define SBL_ERASE_CMD           0x21
#define SBL_DOWNLOAD_CMD        0x24
#define SBL_RESET_CMD           0x25
#define SBL_CHUNK_SIZE          128 
#define SBL_PKT_MAX_LEN         (4 + 1 + SBL_CHUNK_SIZE + 4)
#define MAX_RX_TIMEOUTS         15 // 2 sn * 15 = 30 saniye kesinti toleransı

static const char *TAG = "OTA_MANAGER";

// --- Veri Yapıları ---
typedef enum {
    TARGET_UNKNOWN = 0,
    TARGET_ESP32 = 1,
    TARGET_LOCAL_CC1312 = 2,
    TARGET_REMOTE_CC1312 = 3
} ota_target_device_t;

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_LOCAL_UPDATING
} ota_state_t;

typedef struct {
    uint8_t version[4];
    uint16_t fragment_count;
    uint8_t packet_size;
    uint32_t total_length;
    uint32_t expected_crc;
    struct sockaddr_in server_addr;
    socklen_t server_addr_len;
    ota_target_device_t target;
} ota_session_t;

static ota_state_t current_state = OTA_STATE_IDLE;
static ota_session_t session;
static uint16_t expected_packet_id = 0;
static int udp_sock = -1;
static uint8_t rx_timeout_count = 0;

extern uint8_t* lz77_decompress(const uint8_t *in, int inLen, int *outLen);

// ============================================================================
// 1. ESP32 HTTP OTA TASK
// ============================================================================
void esp_http_ota_task(void *pvParameter) {
    ESP_LOGI(TAG, "ESP32 Kendini Guncelliyor (HTTPS OTA)...");
    
esp_http_client_config_t http_config = {
        .url = "https://sunucu-adresi.com/api/firmware/esp32.bin",
        .cert_pem = NULL, // Üretimde buraya kesinlikle server root CA sertifikasını eklemelisin
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    // ESP-IDF v5 standartlarına uygun OTA sarmalayıcı struct
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        // Gerekirse .http_client_init_cb gibi ek fonksiyonlar buraya eklenebilir
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ESP32 OTA Basarili! Sistem yeniden baslatiliyor...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "ESP32 OTA Basarisiz oldu, Hata: %s", esp_err_to_name(ret));
    }
    vTaskDelete(NULL);
}

// ============================================================================
// 2. YEREL CC1312 BOOTLOADER KONTROLÜ VE FLASHLAMA
// ============================================================================
bool enter_cc1312_bootloader(void) {
    ESP_LOGI(TAG, "UART1 SLIP modundan SBL moduna aliniyor...");
    
    if (uart_is_driver_installed(CC1312_UART_NUM)) {
        uart_driver_delete(CC1312_UART_NUM);
    }
    
    uart_config_t sbl_uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CC1312_UART_NUM, &sbl_uart_config));
    ESP_ERROR_CHECK(uart_driver_install(CC1312_UART_NUM, 1024, 0, 0, NULL, 0));
    uart_flush(CC1312_UART_NUM);

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << CC1312_RESET_PIN) | (1ULL << CC1312_BL_EN_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);

    gpio_set_level(CC1312_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CC1312_BL_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(CC1312_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50)); 
    gpio_set_level(CC1312_BL_EN_PIN, 0);

    ESP_LOGI(TAG, "CC1312 SBL Aktif. UART Senkronizasyonu Bekleniyor.");
    return true;
}

static bool uart_read_byte_with_timeout(uint8_t *byte, int timeout_ms) {
    int len = uart_read_bytes(CC1312_UART_NUM, byte, 1, pdMS_TO_TICKS(timeout_ms));
    return (len == 1);
}

bool write_file_to_cc1312_uart(const char *file_path) {
    FILE *f = fopen(file_path, "rb");
    if (!f) return false;

    uint8_t response;
    uint8_t sync_byte = 0x55;

    uart_write_bytes(CC1312_UART_NUM, (const char*)&sync_byte, 1);
    if (!uart_read_byte_with_timeout(&response, 200) || response != SBL_ACK) {
        ESP_LOGE(TAG, "Auto-baud basarisiz!");
        fclose(f);
        return false;
    }

    uint8_t erase_cmd[] = {SBL_ERASE_CMD, 0x00, 0x00, 0x00, 0x00};
    uart_write_bytes(CC1312_UART_NUM, (const char*)erase_cmd, sizeof(erase_cmd));
    if (!uart_read_byte_with_timeout(&response, 10000) || response != SBL_ACK) {
        ESP_LOGE(TAG, "Flash Erase basarisiz!");
        fclose(f);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t buffer[SBL_CHUNK_SIZE]; 
    uint8_t packet[SBL_PKT_MAX_LEN];
    uint32_t address = 0x00000000;
    size_t bytes_sent = 0;

    while (bytes_sent < file_size) {
        size_t to_read = (file_size - bytes_sent) > SBL_CHUNK_SIZE ? SBL_CHUNK_SIZE : (file_size - bytes_sent);
        fread(buffer, 1, to_read, f);
        
        uint32_t crc = esp_rom_crc32_le(0, buffer, to_read);
        
        int idx = 0;
        packet[idx++] = SBL_DOWNLOAD_CMD;
        packet[idx++] = (address >> 24) & 0xFF;
        packet[idx++] = (address >> 16) & 0xFF;
        packet[idx++] = (address >> 8) & 0xFF;
        packet[idx++] = address & 0xFF;
        packet[idx++] = to_read;
        
        memcpy(&packet[idx], buffer, to_read);
        idx += to_read;
        
        packet[idx++] = (crc >> 24) & 0xFF;
        packet[idx++] = (crc >> 16) & 0xFF;
        packet[idx++] = (crc >> 8) & 0xFF;
        packet[idx++] = crc & 0xFF;

        uart_write_bytes(CC1312_UART_NUM, (const char*)packet, idx);
        
        if (!uart_read_byte_with_timeout(&response, 500) || response != SBL_ACK) {
            ESP_LOGE(TAG, "Download Hatasi! Adres: 0x%08lX", address);
            fclose(f);
            return false;
        }

        bytes_sent += to_read;
        address += to_read;
    }
    fclose(f);

    ESP_LOGI(TAG, "Yazim tamamlandi. Yerel CC1312 Resetleniyor...");
    uint8_t reset_cmd[] = {SBL_RESET_CMD};
    uart_write_bytes(CC1312_UART_NUM, (const char*)reset_cmd, sizeof(reset_cmd));
    
    return true;
}

// SLIP Geri Yükleme Fonksiyonu
void restore_cc1312_slip_uart(void) {
    ESP_LOGI(TAG, "UART1 SBL modundan orijinal SLIP moduna geri donduruluyor...");
    
    if (uart_is_driver_installed(CC1312_UART_NUM)) {
        ESP_ERROR_CHECK(uart_driver_delete(CC1312_UART_NUM));
    }
    
    uart_config_t slip_uart_config = {
        .baud_rate = 115200, // Sistemin asıl SLIP Baud Rate'i
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CC1312_UART_NUM, &slip_uart_config));
    // SLIP için geniş RX buffer tahsisi (Paket drop önleme)
    ESP_ERROR_CHECK(uart_driver_install(CC1312_UART_NUM, 1024 * 4, 0, 0, NULL, 0));
    uart_flush(CC1312_UART_NUM);
    ESP_LOGI(TAG, "SLIP UART arayuzu yeniden aktif edildi.");
}

// ============================================================================
// 3. YEREL CC1312 ASENKRON GÜNCELLEME TASKI
// ============================================================================
static bool verify_firmware_file(void) {
    // (CRC ve Footer doğrulama kodunuz burada kalacak)
    return true; 
}

void local_cc1312_update_task(void *pvParameter) {
    ESP_LOGI(TAG, "Yerel CC1312 Asenkron Guncelleme Basladi...");
    
    if (!verify_firmware_file()) {
        ESP_LOGE(TAG, "Firmware dogrulama basarisiz! Guncelleme iptal edildi.");
        goto cleanup_and_exit;
    }

    FILE *f_in = fopen(FW_FILE_PATH, "rb");
    if (!f_in) {
        ESP_LOGE(TAG, "Guncelleme dosyasi acilamadi!");
        goto cleanup_and_exit;
    }

    fseek(f_in, 0, SEEK_END);
    long f_size = ftell(f_in);
    fseek(f_in, 0, SEEK_SET);

    uint8_t *comp_buf = (uint8_t *)heap_caps_malloc(f_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!comp_buf) {
        ESP_LOGE(TAG, "PSRAM tahsisi basarisiz! Gerekli: %ld byte", f_size);
        fclose(f_in);
        goto cleanup_and_exit;
    }

    fread(comp_buf, 1, f_size, f_in);
    fclose(f_in);

    int decomp_len = 0;
    uint8_t *decomp_data = lz77_decompress(comp_buf, f_size, &decomp_len);

    if (decomp_data) {
        FILE *f_out = fopen(LZ77_OUT_PATH, "wb");
        if (f_out) {
            fwrite(decomp_data, 1, decomp_len, f_out);
            fclose(f_out);
            
            if (enter_cc1312_bootloader()) {
                write_file_to_cc1312_uart(LZ77_OUT_PATH);
            }
            // KRİTİK: İndirme bitse de bitmese de donanımı eski haline döndür
            restore_cc1312_slip_uart();
        }
        heap_caps_free(decomp_data); 
    } else {
        ESP_LOGE(TAG, "LZ77 Decompression basarisiz!");
    }
    heap_caps_free(comp_buf); 

cleanup_and_exit:
    // Hata olsa da başarılı olsa da State Machine'i boşa çıkart!
    current_state = OTA_STATE_IDLE; 
    ESP_LOGI(TAG, "Yerel Guncelleme Taski sonlandiriliyor.");
    vTaskDelete(NULL);
}

// ============================================================================
// 4. UDP HABERLEŞME VE ANA ROUTER TASK 
// ============================================================================
static void send_data_request(uint16_t packet_id) {
    uint8_t req[9];
    req[0] = (OTA_V1 << 4) | OTA_REQUEST;
    req[1] = OTA_CMD_DATA;
    memcpy(&req[2], session.version, 4);
    req[6] = packet_id & 0xFF;         
    req[7] = (packet_id >> 8) & 0xFF;
    req[8] = session.packet_size;
    sendto(udp_sock, req, sizeof(req), 0, (struct sockaddr *)&session.server_addr, session.server_addr_len);
}

static void send_ota_response(uint8_t status_code) {
    uint8_t resp[2];
    resp[0] = (OTA_V1 << 4) | OTA_RESPONSE;
    resp[1] = status_code;
    sendto(udp_sock, resp, sizeof(resp), 0, (struct sockaddr *)&session.server_addr, session.server_addr_len);
}

void udp_ota_task(void *pvParameters) {
    uint8_t rx_buffer[1024];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(UDP_LISTEN_PORT)
    };
    bind(udp_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG, "UDP Yönlendirici Başlatıldı Port: %d", UDP_LISTEN_PORT);
    FILE *fw_file = NULL;

    while (1) {
        int len = recvfrom(udp_sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

        // Zaman Aşımı (Timeout) ve Bağlantı Kopması Durumu
        if (len < 0) {
            if (current_state == OTA_STATE_DOWNLOADING) {
                rx_timeout_count++;
                if (rx_timeout_count >= MAX_RX_TIMEOUTS) {
                    ESP_LOGE(TAG, "OTA Download Zaman Asimi (Sunucu Yanit Vermiyor)! Iptal ediliyor.");
                    if (fw_file) {
                        fclose(fw_file);
                        fw_file = NULL;
                    }
                    current_state = OTA_STATE_IDLE;
                    rx_timeout_count = 0;
                } else {
                    ESP_LOGW(TAG, "Paket %d gecikti, tekrar isteniyor... (Deneme: %d/%d)", expected_packet_id, rx_timeout_count, MAX_RX_TIMEOUTS);
                    send_data_request(expected_packet_id);
                }
            }
            continue;
        }

        rx_timeout_count = 0; // Veri geldiyse zaman aşımı sayacını sıfırla

        // Yerel CC1312 flashlanırken ağdan gelen paketleri drop et/reddet
        if (current_state == OTA_STATE_LOCAL_UPDATING) {
            continue;
        }

        uint8_t msg_type_bits = rx_buffer[0] & 0x0F;
        uint8_t cmd = rx_buffer[1];

        // INFO PAKETİ YAKALAMA
        if (msg_type_bits == OTA_RESPONSE && cmd == OTA_CMD_INFO && current_state == OTA_STATE_IDLE) {
            session.server_addr = source_addr;
            session.server_addr_len = socklen;
            memcpy(session.version, &rx_buffer[2], 4);
            session.target = (ota_target_device_t)session.version[0];

            if (session.target == TARGET_ESP32) {
                xTaskCreate(esp_http_ota_task, "esp_ota_task", 8192, NULL, 5, NULL);
                send_ota_response(OTA_SUCCESS);
                continue; 
            }
            else if (session.target == TARGET_LOCAL_CC1312) {
                fw_file = fopen(FW_FILE_PATH, "wb"); 
                if(!fw_file) {
                    send_ota_response(0x05); 
                    continue;
                }
            }

            session.fragment_count = rx_buffer[6] | (rx_buffer[7] << 8);
            session.packet_size = rx_buffer[8];
            expected_packet_id = 0;
            current_state = OTA_STATE_DOWNLOADING;
            send_data_request(expected_packet_id);
        }
        
        // DATA PAKETİ YAKALAMA
        else if (msg_type_bits == OTA_RESPONSE && cmd == OTA_CMD_DATA && current_state == OTA_STATE_DOWNLOADING) {
            uint16_t rx_pkt_id = rx_buffer[2] | (rx_buffer[3] << 8);
            
            if (rx_pkt_id == expected_packet_id) {
                int data_len = len - 4;
                
                if (session.target == TARGET_LOCAL_CC1312 && fw_file) {
                    fwrite(&rx_buffer[4], 1, data_len, fw_file);
                } 
                else if (session.target == TARGET_REMOTE_CC1312) {
                    uart_write_bytes(CC1312_UART_NUM, (const char *)&rx_buffer[4], data_len);
                }
                
                expected_packet_id++;
                
                if (expected_packet_id < session.fragment_count) {
                    send_data_request(expected_packet_id);
                } else {
                    // İNDİRME TAMAMLANDI
                    if (session.target == TARGET_LOCAL_CC1312) {
                        fclose(fw_file);
                        fw_file = NULL;
                        
                        send_ota_response(OTA_SUCCESS); 
                        
                        current_state = OTA_STATE_LOCAL_UPDATING; 
                        xTaskCreate(local_cc1312_update_task, "cc1312_update", 8192, NULL, 5, NULL);
                    } 
                    else if (session.target == TARGET_REMOTE_CC1312) {
                        send_ota_response(OTA_SUCCESS);
                        current_state = OTA_STATE_IDLE;
                    }
                }
            }
        }
        
        // CANCEL İSTEĞİ
        else if (msg_type_bits == OTA_REQUEST && cmd == OTA_CMD_CANCEL) {
            if (fw_file) {
                fclose(fw_file);
                fw_file = NULL;
            }
            current_state = OTA_STATE_IDLE;
            rx_timeout_count = 0;
            send_ota_response(0x08); 
        }
    }
}