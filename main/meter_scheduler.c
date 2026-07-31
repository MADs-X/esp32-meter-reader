#include "meter_scheduler.h"
#include "meter_db.h"
#include "meter_protocol.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "METER_SCHED";

#define SCHEDULER_PERIOD_MS 300000
static uint8_t seq_counter = 1;

// ------------------------------------------------------------------
// Orijinal Protokole Tam Uyumlu MR_READ_REQUEST Gönderme
// ------------------------------------------------------------------
static bool send_meter_read_request(meter_read_target_t *target) {
    if (meter_sock < 0) {
        ESP_LOGE(TAG, "Cannot send request: meter_sock is not open!");
        return false;
    }

    uint8_t packet[64];
    int offset = 0;

    // 1. Opcode
    packet[offset++] = MR_READ_REQUEST;

    // 2. Flags Hesaplama
    // Bit 0   : lastSucceed (Varsayılan 0)
    // Bit 1   : containsXor
    // Bit 2..6: meterSerialLen
    // Bit 7   : isLongRead
    uint8_t serialLen = strlen(target->meterSerial);
    if (serialLen > 16) serialLen = 16;

    uint8_t flags = 0;
    flags |= (0 & 0x01);                             // lastSucceed
    if (target->hasXorData) flags |= MR_FLAG_CONTAINS_XOR;
    flags |= (serialLen & 0x1F) << 2;               // meterSerialLen
    if (target->isLongRead) flags |= MR_FLAG_LONG_READ;

    packet[offset++] = flags;

    // 3. Sequence Number (1-255 döngüsel)
    packet[offset++] = seq_counter++;
    if (seq_counter == 0) seq_counter = 1;

    // 4. Sayaç Seri Numarası (ASCII)
    for (int i = 0; i < serialLen; i++) {
        packet[offset++] = target->meterSerial[i];
    }

    // 5. Opsiyonel XOR ve Readout Length
    if (target->hasXorData) {
        packet[offset++] = target->xorVal;
        packet[offset++] = (uint8_t)(target->lastReadoutLen >> 8);   // Big-Endian MSB
        packet[offset++] = (uint8_t)(target->lastReadoutLen & 0xFF); // Big-Endian LSB
    }

    // 6. Baud Rate Indikatörü (ASCII '0'-'6')
    packet[offset++] = convert_baud_to_byte(target->startBaudRate);

    // Hedef adres portu ve ailesini garanti et (UDP Port 8008)
    target->destAddr.sin6_family = AF_INET6;
    target->destAddr.sin6_port = htons(8008);

    // UDP Paketini Gönder
    int sent = sendto(meter_sock, packet, offset, 0, 
                      (struct sockaddr *)&target->destAddr, sizeof(struct sockaddr_in6));

    if (sent < 0) {
        ESP_LOGE(TAG, "sendto failed for meter %s (errno: %d)", target->meterSerial, errno);
        return false;
    } else {
        ESP_LOGI(TAG, "Sent MR_READ_REQUEST to %s (Baud: %d, Seq: %d, Len: %d bytes)", 
                 target->meterSerial, target->startBaudRate, seq_counter - 1, offset);
        return true;
    }
}

// ------------------------------------------------------------------
// Periyodik Scheduler Task'ı
// ------------------------------------------------------------------
void meter_scheduler_task(void *pvParameters) {
    TickType_t last_wake = xTaskGetTickCount();
    ESP_LOGI(TAG, "Meter Scheduler Task calisiyor (Periyot: %d ms)...", SCHEDULER_PERIOD_MS);

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SCHEDULER_PERIOD_MS));

        meter_read_target_t target;

        if (get_next_meter_to_read(&target)) {
            ESP_LOGI(TAG, "Okunacak sayac bulundu: %s (Node: %s), İstek gonderiliyor...", 
                     target.meterSerial, target.nodeSerial);

            if (send_meter_read_request(&target)) {
                // İstek gönderildi, cevap bekleniyor. status=false, error_code=0 (henüz hata yok)
                mark_meter_last_access(target.meterSerial, false, 0);
            } else {
                // Gönderim hatası, retry_count artırılacak
                mark_meter_last_access(target.meterSerial, false, -1);
                ESP_LOGE(TAG, "MR_READ_REQUEST gonderilemedi!");
            }
        } else {
            ESP_LOGW(TAG, "Okunacak bekleyen sayac bulunamadi!");
        }
    }
}