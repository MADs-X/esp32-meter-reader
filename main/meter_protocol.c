#include "meter_protocol.h"
#include "meter_db.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <lwip/netdb.h>
#include "esp_timer.h"
#include "esp_random.h"
int meter_sock = -1;  
static const char *TAG = "METER_PROTO";
// meter_protocol.c başında, include'lardan sonra
static void handle_configure(int sock, const uint8_t *data, int len, struct sockaddr_in6 *src_addr);
static void handle_data(int sock, const uint8_t *data, int len, struct sockaddr_in6 *src_addr);
static void handle_error(int sock, const uint8_t *data, int len, struct sockaddr_in6 *src_addr);

void handle_meter_read(int sock, const uint8_t *data, size_t len, struct sockaddr_in6 *src_addr) {
    if (len < 1) return;
    uint8_t opcode = data[0];
    switch (opcode) {
        case MR_CONFIGURE:
            handle_configure(sock, data, (int)len, src_addr);
            break;
        case MR_DATA:
            handle_data(sock, data, (int)len, src_addr);
            break;
        case MR_READ_ERROR:
            handle_error(sock, data, (int)len, src_addr);
            break;
        default:
            ESP_LOGD("METER_PROTO", "Bilinmeyen opcode 0x%02X", opcode);
            break;
    }
}

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
/*burada silinecek
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

            // Paket işleme lojiği - DÜZELTİLDİ: sock parametresi eklendi
            switch (opcode) {
                case MR_CONFIGURE:
                    handle_configure(meter_sock, rx_buffer, len, src);
                    break;
                case MR_DATA:
                    handle_data(meter_sock, rx_buffer, len, src);
                    break;
                case MR_READ_ERROR:
                    handle_error(meter_sock, rx_buffer, len, src);
                    break;
                default:
                    ESP_LOGD(TAG, "Bilinmeyen opcode: 0x%02X", opcode);
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
*/
// ------------------------------------------------------------------
// Paket İşleyicileri
// ------------------------------------------------------------------

static void handle_configure(int sock, const uint8_t *data, int len, struct sockaddr_in6 *src_addr) {
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

    if (readOutLen == 0) {
        ESP_LOGE(TAG, "Gecersiz readOutLen: 0 byte! Bellek tahsisi reddedildi.");
        uint8_t nack = MR_CONFIGURE_NACK;
        sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
        return;
    }

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
            sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
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
            sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
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
            sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
            return;
        }
    }

    // ACK gönder
    uint8_t ack = MR_CONFIGURE_ACK;
    sendto(sock, &ack, 1, 0, (struct sockaddr *)src_addr, sizeof(struct sockaddr_in6));
}

static void handle_data(int sock, const uint8_t *data, int len, struct sockaddr_in6 *src_addr) {
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

static void handle_error(int sock, const uint8_t *data, int len, struct sockaddr_in6 *src_addr) {
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
    int decompLen = 0;
    // PSRAM destekli donanımsal lz77_decompress çağrısı (Önceki planlamamızdaki gibi)
    uint8_t *decompData = lz77_decompress(cd->dataBuffer, cd->readOutLen, &decompLen);
    
    if (!decompData) {
        ESP_LOGE("METER_PROTO", "LZ77 decompress basarisiz! (OOM veya bozuk paket)");
        cd->failed = true;
        return;
    }

    db_write_request_t req;
    strncpy(req.meterSerial, cd->meterSerial, sizeof(req.meterSerial) - 1);
    req.data = decompData;
    req.dataLen = decompLen;
    req.isLongRead = cd->isLongRead;
    req.is_delta = cd->receivingXorData; // XOR Delta bayrağı SQLite task'ına iletiliyor

        if (cd->isLongRead) {
        log_payload_tail(decompData, decompLen);  
    }
    // Bloklamasız (Non-blocking) kuyruk gönderimi
    if (xQueueSend(sensor_queue, &req, 0) != pdTRUE) {
        ESP_LOGE("METER_PROTO", "sensor_queue DOLU! Paket drop edildi.");
        heap_caps_free(decompData);
    }

    cd->completed = true;
    cd->finalAckSent = false;
}

// ------------------------------------------------------------------
// LZ77 Decompress  
// ------------------------------------------------------------------
static const char *LZ77_TAG = "LZ77_HW";

static int read_var_size(int *value, const uint8_t *buf, int ptr, int max_len) {
    int y = 0, numBytes = 0;
    uint8_t b;
    do {
        if (ptr >= max_len) {
            ESP_LOGE(TAG, "VAR_SIZE Taşması! Kötü niyetli/bozuk paket.");
            return -1; // Hata kodu
        }
        b = buf[ptr++];
        y = (y << 7) | (b & 0x7F);
        numBytes++;
    } while ((b & 0x80) != 0);
    
    *value = y;
    return numBytes;
}

uint8_t* lz77_decompress(const uint8_t *in, int inLen, int *outLen) {
    if (in == NULL || inLen < 5) {
        ESP_LOGW(TAG, "Geçersiz giriş boyutu.");
        *outLen = 0;
        return NULL;
    }

    // Big-endian parse
    int outSize = (in[0] << 8) | in[1];
    int inSize  = (in[2] << 8) | in[3];
    
    // Güvenlik: Başlık doğrulaması
    if (inSize != inLen - 4) {
        ESP_LOGW(TAG, "inSize (%d) != inLen-4 (%d). Payload bozuk olabilir.", inSize, inLen-4);
    }

    // OOM Koruması ve PSRAM Kullanımı
    // outSize mantıksız bir seviyedeyse baştan reddet (Örn: max 32KB limiti)
    if (outSize > 32768) {
        ESP_LOGE(TAG, "outSize çok büyük (%d), reddedildi.", outSize);
        *outLen = 0;
        return NULL;
    }
    uint8_t *out = (uint8_t*)heap_caps_malloc(outSize, MALLOC_CAP_DEFAULT);
    
    if (!out) {
        // Tahsis başarısız olursa, Heap durumunu dök (sadece log, task oluşturma YOK)
        ESP_LOGE(TAG, "Hedef buffer tahsisi basarisiz (outSize=%d)", outSize);
        ESP_LOGE(TAG, "Free 8-bit RAM: %u bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
        ESP_LOGE(TAG, "Largest free block: %u bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
            ESP_LOGE(TAG, "Free PSRAM: %u bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        *outLen = 0;
        return NULL;
    }

    uint8_t marker = in[4];
    int inPos = 5;
    int inEnd = inPos + inSize;
    
    // inEnd'in fiziksel diziyi aşmasını engelle
    if (inEnd > inLen) {
        inEnd = inLen;
    }

    int outPos = 0;

    while (inPos < inEnd) {
        uint8_t symbol = in[inPos++];
        
        if (symbol == marker) {
            if (inPos >= inEnd) break; // Paket sonu koruması
            
            if (in[inPos] == 0) {
                if (outPos >= outSize) goto err_overflow;
                out[outPos++] = marker;
                inPos++; 
            } else {
                int length = 0, offset = 0;
                
                int len_bytes = read_var_size(&length, in, inPos, inEnd);
                if (len_bytes < 0) goto err_overflow;
                inPos += len_bytes;
                
                int off_bytes = read_var_size(&offset, in, inPos, inEnd);
                if (off_bytes < 0) goto err_overflow;
                inPos += off_bytes;

                if (offset == 0 || offset > outPos) {
                    ESP_LOGE(TAG, "Geçersiz offset: %d (outPos=%d)", offset, outPos);
                    goto err_cleanup;
                }
                if (outPos + length > outSize) {
                    ESP_LOGE(TAG, "Kopya boyutu buffer sınırını aşıyor.");
                    goto err_cleanup;
                }

                // C'de Overlap (length > offset) durumu byte-by-byte kopyalamada doğal olarak çözülür.
                // memmove KULLANILMAMALIDIR, LZ77 offset overlap mantığı byte dizilimi ister.
                int copySrc = outPos - offset;
                for (int i = 0; i < length; i++) {
                    out[outPos++] = out[copySrc + i];
                }
            }
        } else {
            if (outPos >= outSize) goto err_overflow;
            out[outPos++] = symbol;
        }
    }

    *outLen = outPos;
    
    /*
    if (outPos < outSize) {
        uint8_t *trimmed = heap_caps_realloc(out, outPos, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (trimmed) out = trimmed;
    }
    */
    
    return out;

err_overflow:
    ESP_LOGE(TAG, "Buffer taşması yakalandı.");
err_cleanup:
    heap_caps_free(out);
    *outLen = 0;
    return NULL;
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
// ULID Üretici    
// ------------------------------------------------------------------

static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Inline: 5 Byte'lık ham veriyi (40 bit), 8 karakterlik Base32 Crockford formuna maskeler.
// Döngü branch-prediction hatalarını önler ve CPU registerlarını doğrudan kullanır.
static inline void encode_base32_5bytes(const uint8_t *in, char *out) {
    out[0] = alphabet[(in[0] & 0xF8) >> 3];
    out[1] = alphabet[((in[0] & 0x07) << 2) | ((in[1] & 0xC0) >> 6)];
    out[2] = alphabet[(in[1] & 0x3E) >> 1];
    out[3] = alphabet[((in[1] & 0x01) << 4) | ((in[2] & 0xF0) >> 4)];
    out[4] = alphabet[((in[2] & 0x0F) << 1) | ((in[3] & 0x80) >> 7)];
    out[5] = alphabet[(in[3] & 0x7C) >> 2];
    out[6] = alphabet[((in[3] & 0x03) << 3) | ((in[4] & 0xE0) >> 5)];
    out[7] = alphabet[(in[4] & 0x1F)];
}

/*
 * @brief Thread-safe ULID Generator
 * @param out: Minimum 27 byte'lık önceden tahsis edilmiş buffer
 * @param out_sz: Buffer boyutu (sizeof(out))
 */
esp_err_t generate_ulid(char *out, size_t out_sz) {
    if (out == NULL || out_sz < 27) {
        ESP_LOGE(TAG, "Buffer overflow riski! ULID dizisi min 27 byte olmali.");
        return ESP_ERR_INVALID_ARG;
    }

    // 1. Timestamp (UNIX Epoch ms)
    // DİKKAT: SIMCom LTE uzerinden cihaz zamaninin (SNTP) guncellendiginden emin ol.
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ts = ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);

    uint8_t ts_bytes[6];
    ts_bytes[0] = (ts >> 40) & 0xFF;
    ts_bytes[1] = (ts >> 32) & 0xFF;
    ts_bytes[2] = (ts >> 24) & 0xFF;
    ts_bytes[3] = (ts >> 16) & 0xFF;
    ts_bytes[4] = (ts >> 8) & 0xFF;
    ts_bytes[5] = ts & 0xFF;

    // 2. Hardware Random (80 bit)
    // ESP32'nin donanımsal TRNG (True Random Number Generator) birimi kullanılır.
    uint8_t rand_bytes[10];
    esp_fill_random(rand_bytes, 10);

    // 3. Encode Time (48 bit veri -> 10 karakter)
    // İlk karakter zorunlu olarak en üst 3 biti alır (130-bit alignment gereksinimi).
    out[0] = alphabet[(ts_bytes[0] & 0xE0) >> 5];
    out[1] = alphabet[(ts_bytes[0] & 0x1F)];
    encode_base32_5bytes(&ts_bytes[1], &out[2]);

    // 4. Encode Random (80 bit veri -> 16 karakter)1
    // 10 byte'lık random array, 2 adet 5 byte'lık blok halinde işlenir.
    encode_base32_5bytes(&rand_bytes[0], &out[10]);
    encode_base32_5bytes(&rand_bytes[5], &out[18]);

    // 5. Null Terminator
    out[26] = '\0';

    return ESP_OK;
}