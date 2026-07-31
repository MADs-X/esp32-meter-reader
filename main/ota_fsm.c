#include "ota_fsm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "OTA_FSM";

// --- Donanım Pin Tanımları (CC1312 SBL Kontrolü) ---
#define CC1312_RESET_PIN     GPIO_NUM_4
#define CC1312_BL_EN_PIN     GPIO_NUM_5

// --- FSM Durumları ---
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_FLASHING_CC1312,
    OTA_STATE_FAILED,
    OTA_STATE_SUCCEED
} ota_fsm_state_t;

static ota_fsm_state_t current_state = OTA_STATE_IDLE;

// --- FreeRTOS Objeleri ---
static EventGroupHandle_t ota_event_group;
#define OTA_START_BIT       BIT0
#define OTA_ABORT_BIT       BIT1

static TimerHandle_t guard_timer;
#define GUARD_TIMEOUT_MS    (120 * 1000)

static QueueHandle_t ota_req_queue;

// Harici UART Mutex'i (SLIP taskından import edilmeli)
extern SemaphoreHandle_t uart_slip_mutex;

// Sunucu sertifikası (CMake üzerinden gömülü)
extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");

// --- Guard Timer Callback ---
static void ota_guard_timer_cb(TimerHandle_t xTimer) {
    ESP_LOGE(TAG, "Guard Timer doldu! Islem zaman asimina ugradi.");
    current_state = OTA_STATE_FAILED;
    xEventGroupSetBits(ota_event_group, OTA_ABORT_BIT);
}

// --- CC1312 Donanım Bootloader Tetikleyici ---
static void enter_cc1312_bootloader(void) {
    ESP_LOGI(TAG, "CC1312 SBL moduna aliniyor...");
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CC1312_RESET_PIN) | (1ULL << CC1312_BL_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(CC1312_RESET_PIN, 0);
    gpio_set_level(CC1312_BL_EN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(CC1312_BL_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    gpio_set_level(CC1312_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(CC1312_BL_EN_PIN, 0);
    ESP_LOGI(TAG, "CC1312 SBL aktif. UART bekleniyor.");
}

// --- FSM Task'ı ---
static void ota_supervisor_task(void *pvParameters) {
    ota_request_t current_req;

    while (1) {
        // IDLE durumunda yeni bir talep bekle (Bloklayıcı)
        if (xQueueReceive(ota_req_queue, &current_req, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Yeni OTA talebi alindi. URL: %s", current_req.fw_url);
            
            current_state = OTA_STATE_DOWNLOADING;
            xTimerStart(guard_timer, 0);
            xEventGroupClearBits(ota_event_group, OTA_ABORT_BIT | OTA_START_BIT);

            if (current_req.target == OTA_TARGET_ESP32) {
                // --- ESP32 Native OTA İşlemi ---
                esp_http_client_config_t config = {
                    .url = current_req.fw_url,
                    .cert_pem = (char *)server_cert_pem_start,
                    .timeout_ms = 10000,
                    .keep_alive_enable = true,
                };
                esp_https_ota_config_t ota_config = {
                    .http_config = &config,
                };

                // İndirme işlemi süresince Guard Timer resetlenmeli (HTTP Client Event içinden yapılabilir, burada basitleştirildi)
                esp_err_t ret = esp_https_ota(&ota_config);
                
                if (ret == ESP_OK && !(xEventGroupGetBits(ota_event_group) & OTA_ABORT_BIT)) {
                    current_state = OTA_STATE_SUCCEED;
                } else {
                    current_state = OTA_STATE_FAILED;
                    ESP_LOGE(TAG, "ESP32 OTA Hatasi: %s", esp_err_to_name(ret));
                }

            } else if (current_req.target == OTA_TARGET_CC1312) {
                // --- CC1312 İndirme ve Flashlama İşlemi ---
                ESP_LOGI(TAG, "CC1312 Imajini LittleFS'e indirme basladi (MOCK)...");
                // TODO: HTTP üzerinden LittleFS'e firmware indir ve SHA doğrulamasını yap.
                // Bu aşamada rx_buffer malloc/free ve Guard Timer reset (xTimerReset) işlemleri döngüsel yapılır.
                vTaskDelay(pdMS_TO_TICKS(2000)); // İndirme simülasyonu
                
                if (!(xEventGroupGetBits(ota_event_group) & OTA_ABORT_BIT)) {
                    current_state = OTA_STATE_FLASHING_CC1312;
                    
                    if (xSemaphoreTake(uart_slip_mutex, portMAX_DELAY) == pdTRUE) {
                        enter_cc1312_bootloader();
                        // TODO: TI ROM Bootloader (SBL) protokolü üzerinden 0x55 ile auto-baud, erase ve write chunk işlemleri.
                        vTaskDelay(pdMS_TO_TICKS(3000)); // Flashlama simülasyonu
                        
                        // CC1312 Normal Reset
                        gpio_set_level(CC1312_RESET_PIN, 0);
                        vTaskDelay(pdMS_TO_TICKS(10));
                        gpio_set_level(CC1312_RESET_PIN, 1);
                        
                        xSemaphoreGive(uart_slip_mutex);
                        current_state = OTA_STATE_SUCCEED;
                    } else {
                        ESP_LOGE(TAG, "UART Mutex alinamadi!");
                        current_state = OTA_STATE_FAILED;
                    }
                }
            }

            xTimerStop(guard_timer, 0);

            if (current_state == OTA_STATE_SUCCEED) {
                ESP_LOGI(TAG, "OTA Isleri basarili.");
                if (current_req.target == OTA_TARGET_ESP32) {
                    ESP_LOGW(TAG, "Gateway yeniden baslatiliyor...");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
            } else {
                ESP_LOGE(TAG, "OTA Isleri basarisiz. FSM IDLE durumuna donuyor.");
            }

            current_state = OTA_STATE_IDLE;
        }
    }
}

esp_err_t ota_fsm_init(void) {
    ota_event_group = xEventGroupCreate();
    if (ota_event_group == NULL) return ESP_ERR_NO_MEM;

    ota_req_queue = xQueueCreate(2, sizeof(ota_request_t));
    if (ota_req_queue == NULL) return ESP_ERR_NO_MEM;

    guard_timer = xTimerCreate("ota_guard", pdMS_TO_TICKS(GUARD_TIMEOUT_MS), pdFALSE, (void *)0, ota_guard_timer_cb);
    if (guard_timer == NULL) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreate(ota_supervisor_task, "ota_fsm_task", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "OTA Task olusturulamadi.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA  Modulu baslatildi.");
    return ESP_OK;
}

esp_err_t ota_fsm_trigger_update(const ota_request_t *req) {
    if (req == NULL) return ESP_ERR_INVALID_ARG;
    if (current_state != OTA_STATE_IDLE) {
        ESP_LOGW(TAG, "FSM IDLE degil, talep reddedildi.");
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(ota_req_queue, req, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}