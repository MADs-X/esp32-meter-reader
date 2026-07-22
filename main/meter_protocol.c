#include "meter_protocol.h"
#include "meter_db.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <lwip/netdb.h>
#include "esp_timer.h"

static const char *TAG = "METER_PROTO";

// Global socket (udp_meter_task içinde açılır)
int meter_sock = -1;

// CollectData listesi
#define MAX_COLLECT_DATA 10
static collect_data_t *collect_list[MAX_COLLECT_DATA] = {0};
static int collect_count = 0;

// Kuyruk handle (main.c’de tanımlanır, meter_db.h’de extern)
extern QueueHandle_t sensor_queue;

// ------------------------------------------------------------------
// Prototipler (derleme uyarılarını önlemek için)
// ------------------------------------------------------------------
static void send_mr_miss(collect_data_t *cd, bool final);
static uint8_t* lz77_decompress(const uint8_t *src, int srcLen, int *outLen);

// ------------------------------------------------------------------
// UDP Task (8008)
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// UDP Task (8008)
// ------------------------------------------------------------------
void udp_meter_task(void *pvParameters) {
    int port = (int)(intptr_t)pvParameters;
    
    // Stack tüketimini azaltmak için 1KB buffer'ı dahili SRAM'den tahsis ediyoruz
    uint8_t *rx_buffer = (uint8_t *)heap_caps_malloc(1024, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!rx_buffer) {
        ESP_LOGE(TAG, "RX Buffer malloc basarisiz!");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in6 dest_addr = {0};
    dest_addr.sin6_family = AF_INET6;
    dest_addr.sin6_port = htons(port);
    dest_addr.sin6_addr = in6addr_any;

    meter_sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
    if (meter_sock < 0) {
        ESP_LOGE(TAG, "Socket olusturulamadi port %d", port);
        heap_caps_free(rx_buffer);
        vTaskDelete(NULL);
        return;
    }

    if (bind(meter_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "Bind basarisiz port %d", port);
        close(meter_sock);
        meter_sock = -1;
        heap_caps_free(rx_buffer);
        vTaskDelete(NULL);
        return;
    }

    // LwIP soket okuma zaman aşımı (Timeout). Bloklamayı engeller.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 }; // 500 ms
    if (setsockopt(meter_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGE(TAG, "SO_RCVTIMEO ayarlanamadi, task kilitlenebilir!");
    }

    ESP_LOGI(TAG, "UDP Meter Task baslatildi, port %d", port);
    TickType_t last_check = xTaskGetTickCount();

    while (1) {
        struct sockaddr_storage src_addr;
        socklen_t addr_len = sizeof(src_addr);
        
        // recvfrom çağrısı (rx_buffer sınırı aşılmamalı)
        int len = recvfrom(meter_sock, rx_buffer, 1024 - 1, 0, (struct sockaddr *)&src_addr, &addr_len);
        
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout (500ms). Veri gelmedi. LwIP non-blocking davranışı devrede.
                // State-machine'i işletmeye devam edebiliriz.
            } else {
                ESP_LOGE(TAG, "recvfrom hatasi, errno: %d", errno);
                vTaskDelay(pdMS_TO_TICKS(1000)); // Hata durumunda CPU hogging'i önle
            }
        } else if (len > 0) {
            // Buffer güvenliği için manuel null-termination (0 endeksi string veya flag kontrolleri için kritik)
            rx_buffer[len] = 0;
            
            uint8_t opcode = rx_buffer[0];
            struct sockaddr_in6 *src = (struct sockaddr_in6 *)&src_addr;

            // Paket işleme lojiği
            switch (opcode) {
                case MR_CONFIGURE:
                    handle_configure(rx_buffer, len, src);
                    break;
                case MR_DATA:
                    handle_data(rx_buffer, len, src);
                    break;
                case MR_READ_ERROR:
                    handle_error(rx_buffer, len, src);
                    break;
                default:
                    ESP_LOGW(TAG, "Bilinmeyen opcode: 0x%02X", opcode);
                    break;
            }
        }

        // LwIP timeout'a düşse de düşmese de bu blok garanti olarak işlenir (State-machine bütünlüğü)
        TickType_t now = xTaskGetTickCount();
        if ((now - last_check) >= pdMS_TO_TICKS(1000)) {
            check_all_collect_data();
            last_check = now;
        }
    }

    // Teorik olarak bu bloğa düşmemeli ancak bellek sızıntısına (memory leak) karşı cleanup:
    close(meter_sock);
    meter_sock = -1;
    heap_caps_free(rx_buffer);
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------
// Paket İşleyicileri
// ------------------------------------------------------------------

void handle_configure(const uint8_t *data, int len, struct sockaddr_in6 *src_addr) {
    if (len < 8) {
        ESP_LOGW(TAG, "MR_CONFIGURE çok kısa");
        return;
    }

    int seq = data[1];
    uint16_t readOutLen = (data[2] << 8) | data[3];
    uint8_t totalPacketNum = data[4];
    uint8_t dataBlockLen = data[5];
    uint8_t flags = data[6];
    uint8_t meterSerialLen = data[7];
    bool receivingXorData = flags & 0x01;
    bool isLongRead = flags & 0x02;

    // meterSerial oku
    char meterSerial[16] = {0};
    if (len >= 8 + meterSerialLen) {
        memcpy(meterSerial, &data[8], meterSerialLen);
    } else {
        ESP_LOGW(TAG, "MR_CONFIGURE: meterSerial eksik");
        return;
    }

    // meterFlag oku (opsiyonel)
    char meterFlag[32] = {0};
    int idx = 8 + meterSerialLen;
    int flagIdx = 0;
    while (idx < len && data[idx] != 0 && flagIdx < 31) {
        meterFlag[flagIdx++] = data[idx++];
    }
    if (idx < len && data[idx] == 0) idx++; // \0 atla

    ESP_LOGI(TAG, "MR_CONFIGURE: serial=%s, seq=%d, totalPackets=%d, len=%d, xor=%d, long=%d",
             meterSerial, seq, totalPacketNum, readOutLen, receivingXorData, isLongRead);

    // Daha önce aynı IP'den varsa eski nesneyi bul ve iptal et
    collect_data_t *cd = NULL;
    int old_index = -1;
    for (int i = 0; i < MAX_COLLECT_DATA; i++) {
        if (collect_list[i] && 
            memcmp(&collect_list[i]->srcAddr, src_addr, sizeof(struct sockaddr_in6)) == 0) {
            cd = collect_list[i];
            old_index = i;
            break;
        }
    }
    if (cd) {
        // sequenceNumber farklıysa eskiyi sil
        if (cd->sequenceNumber != seq) {
            free(cd->dataBuffer);
            free(cd->byteMap);
            free(cd);
            cd = NULL;
            collect_list[old_index] = NULL;
            collect_count--;
        }
    }

    if (!cd) {
        cd = calloc(1, sizeof(collect_data_t));
        if (!cd) {
            ESP_LOGE(TAG, "CollectData malloc başarısız");
            uint8_t nack = MR_CONFIGURE_NACK;
            sendto(meter_sock, &nack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
            return;
        }
        memcpy(&cd->srcAddr, src_addr, sizeof(struct sockaddr_in6));
        strcpy(cd->nodeSerial, "node_temp"); // TODO: IP'den nodeSerial üret
        strcpy(cd->meterSerial, meterSerial);
        cd->sequenceNumber = seq;
        cd->receivingXorData = receivingXorData;
        cd->isLongRead = isLongRead;
        cd->readOutLen = readOutLen;
        cd->totalPacketNum = totalPacketNum;
        cd->dataBlockLen = dataBlockLen;
        cd->dataBuffer = calloc(readOutLen, 1);
        cd->byteMap = calloc(totalPacketNum, 1);
        if (!cd->dataBuffer || !cd->byteMap) {
            free(cd->dataBuffer);
            free(cd->byteMap);
            free(cd);
            ESP_LOGE(TAG, "CollectData bellek tahsisi başarısız");
            uint8_t nack = MR_CONFIGURE_NACK;
            sendto(meter_sock, &nack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
            return;
        }
        cd->startTime = xTaskGetTickCount();
        cd->lastPacketTime = cd->startTime;
        cd->completed = false;
        cd->failed = false;
        cd->finalAckSent = false;
        cd->receivedPackets = 0;

        // Listeye ekle
        if (collect_count < MAX_COLLECT_DATA) {
            for (int i = 0; i < MAX_COLLECT_DATA; i++) {
                if (!collect_list[i]) {
                    collect_list[i] = cd;
                    collect_count++;
                    break;
                }
            }
        } else {
            free(cd->dataBuffer);
            free(cd->byteMap);
            free(cd);
            ESP_LOGE(TAG, "CollectData listesi dolu!");
            uint8_t nack = MR_CONFIGURE_NACK;
            sendto(meter_sock, &nack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
            return;
        }
    }

    // ACK gönder
    uint8_t ack = MR_CONFIGURE_ACK;
    sendto(meter_sock, &ack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
}

void handle_data(const uint8_t *data, int len, struct sockaddr_in6 *src_addr) {
    if (len < 2) return;
    uint8_t packetId = data[1];
    int payloadLen = len - 2;

    // İlgili CollectData'yı bul
    collect_data_t *cd = NULL;
    for (int i = 0; i < MAX_COLLECT_DATA; i++) {
        if (collect_list[i] && 
            memcmp(&collect_list[i]->srcAddr, src_addr, sizeof(struct sockaddr_in6)) == 0) {
            cd = collect_list[i];
            break;
        }
    }
    if (!cd) {
        ESP_LOGW(TAG, "MR_DATA için CollectData bulunamadı");
        return;
    }

    // Zaten geldiyse tekrar alma
    if (cd->byteMap[packetId] == 1) {
        ESP_LOGD(TAG, "Paket %d zaten alınmış", packetId);
        return;
    }

    // Veriyi buffer'a kopyala
    int offset = packetId * cd->dataBlockLen;
    if (offset + payloadLen > cd->readOutLen) {
        ESP_LOGE(TAG, "Paket buffer taşması! offset=%d, len=%d", offset, payloadLen);
        return;
    }
    memcpy(cd->dataBuffer + offset, &data[2], payloadLen);
    cd->byteMap[packetId] = 1;
    cd->receivedPackets++;
    cd->lastPacketTime = xTaskGetTickCount();

    ESP_LOGD(TAG, "MR_DATA paketId=%d, alındı (%d/%d)", packetId, cd->receivedPackets, cd->totalPacketNum);

    // Eğer tüm paketler geldiyse hemen işleme al
    if (cd->receivedPackets == cd->totalPacketNum) {
        cd->completed = true;
        ESP_LOGI(TAG, "Tüm paketler alındı, işleniyor...");
        process_completed_data(cd);
    }
}

void handle_error(const uint8_t *data, int len, struct sockaddr_in6 *src_addr) {
    if (len < 2) return;
    uint8_t err = data[1];
    ESP_LOGW(TAG, "MR_READ_ERROR kodu: 0x%02X", err);

    // Bazı hatalarda scheduler reset
    if (err == 0x01 || err == 0x05 || err == 0x06 || err == 0x07) {
        ESP_LOGI(TAG, "Hata nedeniyle scheduler reset (node)");
        // TODO: meter_scheduler reset fonksiyonu çağrılacak
    } else if (err == 0x10) {
        ESP_LOGI(TAG, "Throttled, backoff uygula");
        // TODO: backoff mekanizması
    }
}

// ------------------------------------------------------------------
// CollectData Kontrol ve Zaman Aşımı
// ------------------------------------------------------------------

void check_all_collect_data(void) {
    TickType_t now = xTaskGetTickCount();

    for (int i = 0; i < MAX_COLLECT_DATA; i++) {
        collect_data_t *cd = collect_list[i];
        if (!cd) continue;

        // Zaten tamamlandıysa veya fail olduysa atla
        if (cd->completed || cd->failed) continue;

        // Guard time kontrolü
        if ((now - cd->startTime) >= pdMS_TO_TICKS(GUARD_TIME_MS)) {
            ESP_LOGW(TAG, "Guard time aşıldı, okuma başarısız");
            cd->failed = true;
            // TODO: scheduler'a bildir
            free(cd->dataBuffer);
            free(cd->byteMap);
            free(cd);
            collect_list[i] = NULL;
            collect_count--;
            continue;
        }

        // Packet timeout kontrolü (son paket üzerinden en az 1 paket alınmışsa)
        if (cd->receivedPackets > 0 && 
            (now - cd->lastPacketTime) >= pdMS_TO_TICKS(PACKET_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Packet timeout, eksik parçalar var, bitmap gönderiliyor");
            send_mr_miss(cd, false);
            cd->lastPacketTime = now; // tekrar göndermemek için güncelle
        }

        // Eğer tüm paketler geldiyse (completed) process_completed_data zaten çağrıldı
        // ama belki final ACK gönderilmemiş olabilir, kontrol et
        if (cd->completed && !cd->finalAckSent) {
            send_mr_miss(cd, true);
            cd->finalAckSent = true;
            // Kayıt zaten yapıldı, veriyi temizle
            free(cd->dataBuffer);
            free(cd->byteMap);
            free(cd);
            collect_list[i] = NULL;
            collect_count--;
        }
    }
}

// ------------------------------------------------------------------
// MR_MISS Gönderme (Bitmap)
// ------------------------------------------------------------------

static void send_mr_miss(collect_data_t *cd, bool final) {
    int numBytes = (cd->totalPacketNum + 7) / 8;
    uint8_t *payload = malloc(2 + numBytes);
    if (!payload) {
        ESP_LOGE(TAG, "MR_MISS malloc başarısız");
        return;
    }
    payload[0] = MR_MISS;
    payload[1] = cd->totalPacketNum;

    // Bitmap oluştur (MSB-first)
    int bitPos = 0;
    for (int i = 0; i < numBytes; i++) {
        uint8_t byte = 0;
        for (int b = 7; b >= 0 && bitPos < cd->totalPacketNum; b--, bitPos++) {
            if (cd->byteMap[bitPos] == 1) {
                byte |= (1 << b);
            }
        }
        payload[2 + i] = byte;
    }

    // Gönder
    if (meter_sock >= 0) {
        sendto(meter_sock, payload, 2 + numBytes, 0, 
               (struct sockaddr *)&cd->srcAddr, sizeof(struct sockaddr_in6));
        ESP_LOGI(TAG, "MR_MISS gönderildi (final=%d), %d byte", final, 2 + numBytes);
    } else {
        ESP_LOGE(TAG, "MR_MISS için socket kapalı");
    }
    free(payload);
}

// ------------------------------------------------------------------
// Tamamlanan Veriyi İşleme
// ------------------------------------------------------------------

void process_completed_data(collect_data_t *cd) {
    // 1. LZ77 decompress
    int decompLen = 0;
    uint8_t *decompData = lz77_decompress(cd->dataBuffer, cd->readOutLen, &decompLen);
    if (!decompData) {
        ESP_LOGE(TAG, "LZ77 decompress başarısız");
        cd->failed = true;
        return;
    }

    // 2. XOR kontrolü (temel senaryoda receivingXorData false olduğu için sadece veriyi kontrol et)
    // Şimdilik sadece log
    ESP_LOGI(TAG, "Decompress tamamlandı, boyut: %d", decompLen);

    // 3. Veritabanına kayıt için kuyruğa gönder
    if (sensor_queue == NULL) {
        ESP_LOGE(TAG, "sensor_queue NULL, veri kaybedildi");
        free(decompData);
        return;
    }

    db_write_request_t req;
    strcpy(req.meterSerial, cd->meterSerial);
    req.data = decompData;
    req.dataLen = decompLen;
    req.isLongRead = cd->isLongRead;
    req.isCompressed = false; // zaten decompress edildi

    if (xQueueSend(sensor_queue, &req, 0) != pdTRUE) {
        ESP_LOGE(TAG, "DB kuyruğu dolu, veri kayboldu");
        free(decompData);
    }

    // 4. Final ACK gönder (check_all_collect_data içinde yapılacak)
    cd->completed = true;
    cd->finalAckSent = false; // henüz gönderilmedi
}

// ------------------------------------------------------------------
// LZ77 Decompress (Dummy - gerçek implementasyon ileride)
// ------------------------------------------------------------------

static uint8_t* lz77_decompress(const uint8_t *src, int srcLen, int *outLen) {
    // Şimdilik sadece kopyala
    uint8_t *dst = malloc(srcLen);
    if (!dst) return NULL;
    memcpy(dst, src, srcLen);
    *outLen = srcLen;
    return dst;
}

// ------------------------------------------------------------------
// Baud Rate Dönüşümleri
// ------------------------------------------------------------------

uint8_t convert_baud_to_byte(int baud) {
    switch (baud) {
        case 600:   return 0x31;
        case 1200:  return 0x32;
        case 2400:  return 0x33;
        case 4800:  return 0x34;
        case 9600:  return 0x35;
        case 19200: return 0x36;
        case 300:
        default:    return 0x30;
    }
}

int convert_byte_to_baud(uint8_t indicator) {
    switch (indicator) {
        case 0x31: return 600;
        case 0x32: return 1200;
        case 0x33: return 2400;
        case 0x34: return 4800;
        case 0x35: return 9600;
        case 0x36: return 19200;
        case 0x30:
        default:    return 300;
    }
}

// ------------------------------------------------------------------
// ULID Üretici (basit)
// ------------------------------------------------------------------

void generate_ulid(char *out) {
    static const char *alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint64_t ts = esp_timer_get_time() / 1000; // ms
    uint8_t rand[10];
    esp_fill_random(rand, 10);
    // Basit: random ile doldur
    for (int i = 0; i < 26; i++) {
        out[i] = alphabet[rand[i % 10] % 32];
    }
    out[26] = 0;
}