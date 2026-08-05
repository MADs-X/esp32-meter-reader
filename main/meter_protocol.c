#include "meter_protocol.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "meter_db.h"
#include "obis_manager.h"
#include <esp_log.h>
#include <lwip/netdb.h>
#include <stdlib.h>
#include <string.h>
int meter_sock = -1;
static const char *TAG = "METER_PROTO";
// meter_protocol.c başında, include'lardan sonra
static void handle_configure(int sock, const uint8_t *data, int len,
                             struct sockaddr_in6 *src_addr);
static void handle_data(int sock, const uint8_t *data, int len,
                        struct sockaddr_in6 *src_addr);
static void handle_error(int sock, const uint8_t *data, int len,
                         struct sockaddr_in6 *src_addr);
static void send_mr_miss(collect_data_t *cd, bool final);
static uint8_t *lz77_decompress(const uint8_t *src, int srcLen, int *outLen);

// CollectData listesi
#define MAX_COLLECT_DATA 10
static collect_data_t *collect_list[MAX_COLLECT_DATA] = {0};
static int collect_count = 0;

extern QueueHandle_t sensor_queue;

void handle_meter_read(int sock, const uint8_t *data, size_t len,
                       struct sockaddr_in6 *src_addr) {
  if (len < 1)
    return;
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
  case MR_LOAD_REQUEST: {
    ESP_LOGI(TAG, "Sunucudan Yük Profili Istegi Geldi.");
    process_load_read_request(data, len, src_addr);
    break;
  }
  case MR_OBIS_RESPONSE: {
    // 1. Minimum uzunluk kontrolü
    if (len < 4) {
      ESP_LOGE(TAG, "HATA: OBIS Response paketi cok kisa!");
      break;
    }

    // 2. Metre seri numarası uzunluğunu çıkar (data[1] >> 2)
    uint8_t meter_len = data[1] >> 2;
    if (meter_len == 0 || meter_len > 16 || 4 + meter_len > len) {
      ESP_LOGE(TAG, "HATA: Gecersiz meter_len (%d) veya paket boyutu yetersiz!",
               meter_len);
      break;
    }

    // 3. Header verilerini oku
    uint8_t seq = data[2];
    uint8_t frameInfo = data[3];
    uint8_t frameCount = (frameInfo >> 4) & 0x0F;
    uint8_t frameId = frameInfo & 0x0F;

    if (frameCount == 0)
      frameCount = 1; // Güvenlik

    // 4. Metre Seri Numarasını Güvenle Çıkar (Null-Terminated)
    char meterSerial[17] = {0};
    memcpy(meterSerial, &data[4], meter_len);
    meterSerial[meter_len] = '\0';

    // 5. Payload ve OBIS Sayısını Hesapla
    int payloadOffset = 4 + meter_len;
    uint16_t payloadLen = len - payloadOffset;

    uint8_t obisCount = 0;
    int temp_idx = payloadOffset;
    while (temp_idx + 2 <= len) {
      uint8_t o_len = data[temp_idx];
      temp_idx += 2 + o_len;
      obisCount++;
    }

    ESP_LOGI(TAG,
             "OBIS Response Decode: meter=%s, seq=%d, frames=%d, id=%d, "
             "obisCount=%d",
             meterSerial, seq, frameCount, frameId, obisCount);

    // 6. nodeSerial pakette bulunmadiği için NULL gönderiyoruz. (Manager
    // içeride eşleyecek)
    obis_process_incoming_packet(NULL, meterSerial, seq, frameCount, frameId,
                                 obisCount, &data[payloadOffset], payloadLen);
    break;
  }
  default:
    ESP_LOGD("METER_PROTO", "Bilinmeyen opcode 0x%02X", opcode);
    break;
  }
}
// ------------------------------------------------------------------
// Paket İşleyicileri
// ------------------------------------------------------------------

static void handle_configure(int sock, const uint8_t *data, int len,
                             struct sockaddr_in6 *src_addr) {
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
  if (idx < len && data[idx] == 0)
    idx++; // \0 atla

  ESP_LOGI(TAG,
           "MR_CONFIGURE: serial=%s, seq=%d, totalPackets=%d, len=%d, xor=%d, "
           "long=%d",
           meterSerial, seq, totalPacketNum, readOutLen, receivingXorData,
           isLongRead);

  if (readOutLen == 0) {
    ESP_LOGE(TAG, "Gecersiz readOutLen: 0 byte! Bellek tahsisi reddedildi.");
    uint8_t nack = MR_CONFIGURE_NACK;
    sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr,
           sizeof(struct sockaddr_in6));
    return;
  }

  // Daha önce aynı IP'den varsa eski nesneyi bul ve iptal et
  collect_data_t *cd = NULL;
  int old_index = -1;
  for (int i = 0; i < MAX_COLLECT_DATA; i++) {
    if (collect_list[i] && memcmp(&collect_list[i]->srcAddr, src_addr,
                                  sizeof(struct sockaddr_in6)) == 0) {
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
      sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr,
             sizeof(struct sockaddr_in6));
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
      sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr,
             sizeof(struct sockaddr_in6));
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
      sendto(sock, &nack, 1, 0, (struct sockaddr *)src_addr,
             sizeof(struct sockaddr_in6));
      return;
    }
  }

  // ACK gönder
  uint8_t ack = MR_CONFIGURE_ACK;
  sendto(sock, &ack, 1, 0, (struct sockaddr *)src_addr,
         sizeof(struct sockaddr_in6));
}

static void handle_data(int sock, const uint8_t *data, int len,
                        struct sockaddr_in6 *src_addr) {
  if (len < 2)
    return;
  uint8_t packetId = data[1];
  int payloadLen = len - 2;

  // İlgili CollectData'yı bul
  collect_data_t *cd = NULL;
  for (int i = 0; i < MAX_COLLECT_DATA; i++) {
    if (collect_list[i] && memcmp(&collect_list[i]->srcAddr, src_addr,
                                  sizeof(struct sockaddr_in6)) == 0) {
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
    ESP_LOGE(TAG, "Paket buffer taşması! offset=%d, len=%d", offset,
             payloadLen);
    return;
  }
  memcpy(cd->dataBuffer + offset, &data[2], payloadLen);
  cd->byteMap[packetId] = 1;
  cd->receivedPackets++;
  cd->lastPacketTime = xTaskGetTickCount();

  ESP_LOGD(TAG, "MR_DATA paketId=%d, alındı (%d/%d)", packetId,
           cd->receivedPackets, cd->totalPacketNum);

  // Eğer tüm paketler geldiyse hemen işleme al
  if (cd->receivedPackets == cd->totalPacketNum) {
    cd->completed = true;
    ESP_LOGI(TAG, "Tüm paketler alındı, işleniyor...");
    process_completed_data(cd);
  }
}

static void handle_error(int sock, const uint8_t *data, int len,
                         struct sockaddr_in6 *src_addr) {
  if (len < 2)
    return;
  uint8_t err = data[1];
  ESP_LOGW(TAG, "MR_READ_ERROR kodu: 0x%02X", err);

  // Hata geldiğinde OBIS FSM durumunu güncelle
  obis_set_state_by_addr(src_addr, OBIS_STATE_READ_ERROR);

  // İlgili CollectData'yı bul, sayaç durumunu güncelle ve temizle
  for (int i = 0; i < MAX_COLLECT_DATA; i++) {
    if (collect_list[i] && memcmp(&collect_list[i]->srcAddr, src_addr,
                                  sizeof(struct sockaddr_in6)) == 0) {

      collect_data_t *cd = collect_list[i];
      ESP_LOGW(TAG,
               "Hata alindi: sayac=%s, hata=0x%02X. CollectData temizleniyor.",
               cd->meterSerial, err);

      // Sayaç durumunu güncelle (başarısız okuma)
      mark_meter_last_access(cd->meterSerial, false, err);

      // CollectData'yı temizle
      free(cd->dataBuffer);
      free(cd->byteMap);
      free(cd);
      collect_list[i] = NULL;
      collect_count--;
      break;
    }
  }

  if (err == 0x01 || err == 0x05 || err == 0x06 || err == 0x07) {
    ESP_LOGI(TAG, "Hata nedeniyle scheduler reset (node)");
  } else if (err == 0x10) {
    ESP_LOGI(TAG, "Throttled, backoff uygula");
  }
}

// ------------------------------------------------------------------
// CollectData Kontrol ve Zaman Aşımı
// ------------------------------------------------------------------

void check_all_collect_data(void) {
  TickType_t now = xTaskGetTickCount();

  for (int i = 0; i < MAX_COLLECT_DATA; i++) {
    collect_data_t *cd = collect_list[i];
    if (!cd)
      continue;

    // 1. Zaten başarısız olan kayıtların temizliği
    if (cd->failed) {
      free(cd->dataBuffer);
      free(cd->byteMap);
      free(cd);
      collect_list[i] = NULL;
      collect_count--;
      continue;
    }

    // 2. Guard Time Aşımı (Genel Zaman Aşımı)
    if (!cd->completed &&
        (now - cd->startTime) >= pdMS_TO_TICKS(GUARD_TIME_MS)) {
      ESP_LOGW(TAG, "Guard time asildi (%s), okuma basarisiz!",
               cd->meterSerial);
      cd->failed = true;
      mark_meter_last_access(cd->meterSerial, false, 0x01); // Read Timeout
      free(cd->dataBuffer);
      free(cd->byteMap);
      free(cd);
      collect_list[i] = NULL;
      collect_count--;
      continue;
    }

    // 3. Paket İçi Zaman Aşımı (Eksik Parçalar İçin Bitmap İsteği)
    if (!cd->completed && cd->receivedPackets > 0 &&
        (now - cd->lastPacketTime) >= pdMS_TO_TICKS(PACKET_TIMEOUT_MS)) {
      ESP_LOGW(TAG,
               "Packet timeout (%s), eksik parcalar var, bitmap gonderiliyor",
               cd->meterSerial);
      send_mr_miss(cd, false);
      cd->lastPacketTime = now;
    }

    // 4. Tamamlanan Okumalar İçin Final ACK ve Bellek Temizliği
    if (cd->completed && !cd->finalAckSent) {
      send_mr_miss(cd, true);
      cd->finalAckSent = true;
      ESP_LOGI(TAG, "Final ACK gonderildi, CollectData temizleniyor: %s",
               cd->meterSerial);
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
    ESP_LOGI(TAG, "MR_MISS gönderildi (final=%d), %d byte", final,
             2 + numBytes);
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
  // PSRAM destekli donanımsal lz77_decompress çağrısı (Önceki planlamamızdaki
  // gibi)
  uint8_t *decompData =
      lz77_decompress(cd->dataBuffer, cd->readOutLen, &decompLen);

  if (!decompData) {
    ESP_LOGE("METER_PROTO",
             "LZ77 decompress basarisiz! (OOM veya bozuk paket)");
    cd->failed = true;
    return;
  }

  db_write_request_t req;
  strncpy(req.meterSerial, cd->meterSerial, sizeof(req.meterSerial) - 1);
  req.data = decompData;
  req.dataLen = decompLen;
  req.isLongRead = cd->isLongRead;
  req.is_delta =
      cd->receivingXorData; // XOR Delta bayrağı SQLite task'ına iletiliyor

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

uint8_t *lz77_decompress(const uint8_t *in, int inLen, int *outLen) {
  if (in == NULL || inLen < 5) {
    ESP_LOGW(TAG, "Geçersiz giriş boyutu.");
    *outLen = 0;
    return NULL;
  }

  // Big-endian parse
  int outSize = (in[0] << 8) | in[1];
  int inSize = (in[2] << 8) | in[3];

  // Güvenlik: Başlık doğrulaması
  if (inSize != inLen - 4) {
    ESP_LOGW(TAG, "inSize (%d) != inLen-4 (%d). Payload bozuk olabilir.",
             inSize, inLen - 4);
  }

  // OOM Koruması ve PSRAM Kullanımı
  // outSize mantıksız bir seviyedeyse baştan reddet (Örn: max 32KB limiti)
  if (outSize > 32768) {
    ESP_LOGE(TAG, "outSize çok büyük (%d), reddedildi.", outSize);
    *outLen = 0;
    return NULL;
  }
  uint8_t *out = (uint8_t *)heap_caps_malloc(outSize, MALLOC_CAP_DEFAULT);

  if (!out) {
    // Tahsis başarısız olursa, Heap durumunu dök (sadece log, task oluşturma
    // YOK)
    ESP_LOGE(TAG, "Hedef buffer tahsisi basarisiz (outSize=%d)", outSize);
    ESP_LOGE(TAG, "Free 8-bit RAM: %u bytes",
             heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGE(TAG, "Largest free block: %u bytes",
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
      ESP_LOGE(TAG, "Free PSRAM: %u bytes",
               heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
      if (inPos >= inEnd)
        break; // Paket sonu koruması

      if (in[inPos] == 0) {
        if (outPos >= outSize)
          goto err_overflow;
        out[outPos++] = marker;
        inPos++;
      } else {
        int length = 0, offset = 0;

        int len_bytes = read_var_size(&length, in, inPos, inEnd);
        if (len_bytes < 0)
          goto err_overflow;
        inPos += len_bytes;

        int off_bytes = read_var_size(&offset, in, inPos, inEnd);
        if (off_bytes < 0)
          goto err_overflow;
        inPos += off_bytes;

        if (offset == 0 || offset > outPos) {
          ESP_LOGE(TAG, "Geçersiz offset: %d (outPos=%d)", offset, outPos);
          goto err_cleanup;
        }
        if (outPos + length > outSize) {
          ESP_LOGE(TAG, "Kopya boyutu buffer sınırını aşıyor.");
          goto err_cleanup;
        }

        // C'de Overlap (length > offset) durumu byte-by-byte kopyalamada doğal
        // olarak çözülür. memmove KULLANILMAMALIDIR, LZ77 offset overlap
        // mantığı byte dizilimi ister.
        int copySrc = outPos - offset;
        for (int i = 0; i < length; i++) {
          out[outPos++] = out[copySrc + i];
        }
      }
    } else {
      if (outPos >= outSize)
        goto err_overflow;
      out[outPos++] = symbol;
    }
  }

  *outLen = outPos;

  /*
  if (outPos < outSize) {
      uint8_t *trimmed = heap_caps_realloc(out, outPos, MALLOC_CAP_8BIT |
  MALLOC_CAP_SPIRAM); if (trimmed) out = trimmed;
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
  case 600:
    return 0x31;
  case 1200:
    return 0x32;
  case 2400:
    return 0x33;
  case 4800:
    return 0x34;
  case 9600:
    return 0x35;
  case 19200:
    return 0x36;
  case 300:
  default:
    return 0x30;
  }
}

int convert_byte_to_baud(uint8_t indicator) {
  switch (indicator) {
  case 0x31:
    return 600;
  case 0x32:
    return 1200;
  case 0x33:
    return 2400;
  case 0x34:
    return 4800;
  case 0x35:
    return 9600;
  case 0x36:
    return 19200;
  case 0x30:
  default:
    return 300;
  }
}

// ------------------------------------------------------------------
// ULID Üretici
// ------------------------------------------------------------------

static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Inline: 5 Byte'lık ham veriyi (40 bit), 8 karakterlik Base32 Crockford
// formuna maskeler. Döngü branch-prediction hatalarını önler ve CPU
// registerlarını doğrudan kullanır.
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
  // DİKKAT: SIMCom LTE uzerinden cihaz zamaninin (SNTP) guncellendiginden emin
  // ol.
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint64_t ts =
      ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);

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
  // İlk karakter zorunlu olarak en üst 3 biti alır (130-bit alignment
  // gereksinimi).
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

bool is_meter_read_in_progress_by_addr(const struct sockaddr_in6 *src_addr) {
  if (!src_addr)
    return false;
  for (int i = 0; i < MAX_COLLECT_DATA; i++) {
    if (collect_list[i] && !collect_list[i]->completed &&
        !collect_list[i]->failed) {
      if (memcmp(&collect_list[i]->srcAddr.sin6_addr, &src_addr->sin6_addr,
                 sizeof(struct in6_addr)) == 0) {
        return true;
      }
    }
  }
  return false;
}
// ------------------------------------------------------------------
// Periyodik Okuma Parser Fonksiyonları
// ------------------------------------------------------------------

void process_periodic_obis_conf(const uint8_t *data, uint16_t len,
                                struct sockaddr_in6 *source_addr) {
  if (len < 7)
    return;

  uint8_t packet_count = data[1] & 0x07;
  uint8_t meter_serial_len = (data[1] >> 3) & 0x1F;
  uint8_t seq_num = data[2];
  uint8_t baud_rate = data[3];
  uint8_t total_obis = data[4];
  uint8_t interval = data[5];
  uint8_t total_len = data[6];

  char meter_serial[32] = {0};
  if (meter_serial_len > 0 && meter_serial_len < 32 &&
      7 + meter_serial_len <= len) {
    memcpy(meter_serial, &data[7], meter_serial_len);
  } else
    return;

  // OBIS Manager'a görevi ekle
  bool success =
      obis_mgr_add_periodic_task(meter_serial, baud_rate, interval, total_obis,
                                 total_len, seq_num, packet_count);

  if (success) {
    // ACK Gönder (0x15)
    uint8_t ack_buf[64];
    int ack_idx = 0;
    ack_buf[ack_idx++] = PERIODIC_OBIS_CONF_ACK;
    ack_buf[ack_idx++] = data[1];
    ack_buf[ack_idx++] = seq_num;
    ack_buf[ack_idx++] = baud_rate;
    ack_buf[ack_idx++] = total_obis;
    ack_buf[ack_idx++] = interval;
    ack_buf[ack_idx++] = total_len;
    memcpy(&ack_buf[ack_idx], meter_serial, meter_serial_len);
    ack_idx += meter_serial_len;

    sendto(meter_sock, ack_buf, ack_idx, 0, (struct sockaddr *)source_addr,
           sizeof(struct sockaddr_in6));
  }
}

void process_periodic_obis_packet(const uint8_t *data, uint16_t len,
                                  struct sockaddr_in6 *source_addr) {
  if (len < 5)
    return;

  uint8_t obis_count_in_packet = data[1] & 0x07;
  uint8_t meter_serial_len = (data[1] >> 3) & 0x1F;

  char meter_serial[32] = {0};
  if (meter_serial_len > 0 && meter_serial_len < 32 &&
      4 + meter_serial_len <= len) {
    memcpy(meter_serial, &data[4], meter_serial_len);
  } else
    return;

  int offset = 4 + meter_serial_len;
  for (int k = 0; k < obis_count_in_packet; k++) {
    if (offset + 1 >= len)
      break;
    uint8_t obis_len = data[offset++];
    uint8_t obis_index = data[offset++];

    if (offset + obis_len > len)
      break;

    char obis_str[32] = {0};
    if (obis_len < sizeof(obis_str)) {
      memcpy(obis_str, &data[offset], obis_len);
      // OBIS Manager'a listeye eklemesi için pasla
      obis_mgr_add_periodic_obis(meter_serial, obis_str);
    }
    offset += obis_len;
  }

  // ACK Gönder (0x17)
  uint8_t ack_buf[64];
  int ack_idx = 0;
  ack_buf[ack_idx++] = PERIODIC_OBIS_PACKET_ACK;
  ack_buf[ack_idx++] = data[1];
  ack_buf[ack_idx++] = data[2];
  memcpy(&ack_buf[ack_idx], meter_serial, meter_serial_len);
  ack_idx += meter_serial_len;
  ack_buf[ack_idx++] = 0x05; // Başarılı

  sendto(meter_sock, ack_buf, ack_idx, 0, (struct sockaddr *)source_addr,
         sizeof(struct sockaddr_in6));
}

void process_delete_periodic_obis(const uint8_t *data, uint16_t len,
                                  struct sockaddr_in6 *source_addr) {
  if (len < 3)
    return;

  uint8_t meter_serial_len = (data[1] >> 2) & 0x1F;
  char meter_serial[32] = {0};
  if (meter_serial_len > 0 && meter_serial_len < 32 &&
      3 + meter_serial_len <= len) {
    memcpy(meter_serial, &data[3], meter_serial_len);
  }

  // OBIS Manager'dan görevi sil
  obis_mgr_delete_periodic_task(meter_serial);

  // ACK Gönder (0x19)
  uint8_t ack_buf[64];
  ack_buf[0] = DELETE_PERIODIC_OBIS_CODES_ACK;
  ack_buf[1] = data[1];
  ack_buf[2] = data[2];
  memcpy(&ack_buf[3], meter_serial, meter_serial_len);
  ack_buf[3 + meter_serial_len] = 0x05;

  sendto(meter_sock, ack_buf, 4 + meter_serial_len, 0,
         (struct sockaddr *)source_addr, sizeof(struct sockaddr_in6));
}