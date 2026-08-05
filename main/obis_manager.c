#include "obis_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "meter_protocol.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "OBIS_MGR";
periodic_task_t periodic_tasks[MAX_PERIODIC_METERS] = {0};
static obis_reader_t obis_pool[OBIS_MAX_NODES];
static SemaphoreHandle_t obis_pool_mutex = NULL;
static int udp_sock = -1;

// ============================================================
// VERİTABANI BAĞLANTI STUB'LARI
// ============================================================
static const char *db_get_next_meter_serial(const char *node_serial) {
  static const char *dummy_meter = "BYL340001360";
  (void)node_serial;
  return dummy_meter;
}

static void db_save_obis_read(const char *meter_serial, const char *obis_code,
                              const char *value) {
  ESP_LOGI(TAG, "DB KAYIT -> SAYAC: %s | OBIS: %s | Değer: %s", meter_serial,
           obis_code, value);
}

static void obis_reset_reader(obis_reader_t *reader) {
  // Java'daki Guard Task mantığı: FSM durumlarını sıfırla.
  reader->send_req_counter = 0;
  reader->send_req_limit = 1;
  reader->total_request_attempts = 0;
  reader->wait_counter = 0;
  reader->seq_number = 1 + (reader->seq_number % 31);

  // Yeni metre bilgisini SADECE periyot başında al
  const char *next_meter = db_get_next_meter_serial(reader->node_serial);
  if (next_meter != NULL) {
    if (strncmp(reader->info.meter_serial, next_meter, SERIAL_MAX_LEN) != 0) {
      strncpy(reader->info.meter_serial, next_meter, SERIAL_MAX_LEN - 1);
      reader->info.meter_serial[SERIAL_MAX_LEN - 1] = '\0';
    }
    reader->state = OBIS_STATE_REQUESTING;
  } else {
    reader->state = OBIS_STATE_WAITING;
  }

  // Cache ve bayrak temizliği
  for (int i = 0; i < reader->info.active_code_count; i++) {
    reader->info.codes[i].is_received = false;
    reader->info.codes[i].frame_count = 0;
    reader->info.codes[i].received_frame_mask = 0;
    memset(reader->info.codes[i].frame_buffers, 0,
           sizeof(reader->info.codes[i].frame_buffers));
    memset(reader->info.codes[i].frame_lens, 0,
           sizeof(reader->info.codes[i].frame_lens));
    memset(reader->info.codes[i].value_buf, 0,
           sizeof(reader->info.codes[i].value_buf));
  }
}

// Ağı yormamak adına sadece buffer oluşturur, Mutex kilitliyken hızlıca
// çağrılır
static int obis_build_request_packet(obis_reader_t *reader, uint8_t *req) {
  int offset = 0;
  int meter_len = strnlen(reader->info.meter_serial, SERIAL_MAX_LEN);

  req[offset++] = MR_OBIS_REQUEST;
  req[offset++] = (uint8_t)(meter_len << 2);
  req[offset++] = (uint8_t)(reader->seq_number & 0xFF);

  int count_idx = offset++;
  int len_idx = offset++;

  req[offset++] = reader->info.start_baud_rate;

  memcpy(&req[offset], reader->info.meter_serial, meter_len);
  offset += meter_len;

  int code_count = 0;
  int total_len = 0;
  uint8_t current_dynamic_index = 0; // 0'dan başlayarak dinamik numara üret

  for (int i = 0; i < reader->info.active_code_count; i++) {
    if (reader->info.codes[i].is_received)
      continue; // Alınanları atla

    int obis_len = strnlen(reader->info.codes[i].code, OBIS_CODE_MAX_LEN);
    int item_len = 2 + obis_len;

    // 62 Byte (OBIS_MAX_REQUEST_LEN) sınırını aşıyor mu?
    if (offset + item_len <= OBIS_MAX_REQUEST_LEN) {
      total_len += item_len;
      code_count++;

      // Ürettiğimiz anlık indeksi struct'a kaydet (Cevap gelince eşleştirmek
      // için)
      reader->info.codes[i].temp_index = current_dynamic_index;

      req[offset++] = (uint8_t)obis_len;
      req[offset++] = (uint8_t)current_dynamic_index;
      memcpy(&req[offset], reader->info.codes[i].code, obis_len);
      offset += obis_len;

      current_dynamic_index++;
    } else {
      // Paket doldu! Kalanlar bir sonraki chunk (parça) gönderimine kalacak.
      break;
    }
  }

  if (code_count == 0)
    return 0;
  req[count_idx] = (uint8_t)code_count;
  req[len_idx] = (uint8_t)total_len;

  return offset;
}

// ============================================================
// OBIS FSM TASK
// ============================================================
void obis_manager_task(void *pvParameters) {
  ESP_LOGI(TAG, "OBIS FSM Task Baslatildi (Periyot: 10sn)");

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(10000);

  while (1) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    for (int i = 0; i < OBIS_MAX_NODES; i++) {
      // DİKKAT: Mutex'i BÜTÜN DÖNGÜ yerine SADECE İLGİLİ NODE durumu çekmek
      // için alıyoruz.
      if (xSemaphoreTake(obis_pool_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        continue;

      obis_reader_t *reader = &obis_pool[i];
      if (!reader->is_active) {
        xSemaphoreGive(obis_pool_mutex);
        continue;
      }

      reader->remaining_time_sec -= 10;
      if (reader->remaining_time_sec <= 0) {
        reader->remaining_time_sec = reader->read_period;
        obis_reset_reader(reader);
      }

      bool should_send = false;
      uint8_t req[OBIS_MAX_REQUEST_LEN] = {0};
      int req_len = 0;
      struct sockaddr_in6 dest_addr;
      char node_serial[SERIAL_MAX_LEN];

      switch (reader->state) {
      case OBIS_STATE_REQUESTING:
        reader->send_req_counter++;
        if (reader->send_req_counter >= reader->send_req_limit) {
          reader->send_req_counter = 0;
          if (reader->info.active_code_count > 0) {
            req_len = obis_build_request_packet(reader, req);
            if (req_len > 0) {
              should_send = true;
              dest_addr = reader->node_addr; // Mutex içindeyken güvenle kopyala
              strncpy(node_serial, reader->node_serial, SERIAL_MAX_LEN);
            } else {
              reader->state = OBIS_STATE_WAITING;
            }
          } else {
            reader->state = OBIS_STATE_WAITING;
          }
        }
        break;
      case OBIS_STATE_PROCESSING:
        if (reader->wait_counter >= 4)
          reader->state = OBIS_STATE_FAILED;
        if (reader->wait_counter > 0)
          reader->wait_counter++;
        break;
      case OBIS_STATE_SUCCEED:
      case OBIS_STATE_FAILED:
      case OBIS_STATE_READ_ERROR:
        reader->state = OBIS_STATE_WAITING;
        break;
      case OBIS_STATE_WAITING:
      default:
        break;
      }

      xSemaphoreGive(obis_pool_mutex);

      if (should_send && req_len > 0) {
        if (is_meter_read_in_progress_by_addr(&dest_addr)) {
          continue;
        }

        int err = sendto(udp_sock, req, req_len, 0,
                         (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err > 0) {
          ESP_LOGI(TAG, "OR: %s -> Request sent! (Len: %d)", node_serial,
                   req_len);

          // Başarılı gönderim sonrası limitleri artırmak için Mutex'i kısa
          // süreliğine geri al
          if (xSemaphoreTake(obis_pool_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (reader->send_req_limit == 1)
              reader->send_req_limit = 3;
            else if (reader->send_req_limit < 30)
              reader->send_req_limit *= 3;
            xSemaphoreGive(obis_pool_mutex);
          }
        } else {
          ESP_LOGE(TAG, "Socket Send Failed! errno: %d", errno);
        }
      }
    }
  }
}

void obis_process_incoming_packet(const char *node_serial,
                                  const char *meter_serial, uint16_t seq,
                                  uint8_t frame_count, uint8_t frame_id,
                                  uint8_t obis_count, const uint8_t *data,
                                  uint16_t data_len) {

  if (xSemaphoreTake(obis_pool_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGE(TAG, "OBIS Mutex alinamadi, RX paketi drop edildi!");
    return;
  }

  obis_reader_t *reader = NULL;
  for (int i = 0; i < OBIS_MAX_NODES; i++) {
    if (!obis_pool[i].is_active)
      continue;

    // SADECE SAYAC SERİ NUMARASINA GÖRE EŞLEŞTİRME YAPIYORUZ
    if (strncmp(obis_pool[i].info.meter_serial, meter_serial, SERIAL_MAX_LEN) ==
        0) {

      reader = &obis_pool[i];
      break; // Doğru sayacı bulduk, döngüden çık
    }
  }

  if (reader == NULL) {
    ESP_LOGW(TAG, "Eslenen OBIS reader bulunamadi (Meter: %s, Seq: %d)",
             meter_serial, seq);
    xSemaphoreGive(obis_pool_mutex);
    return;
  }

  bool wait = (frame_id != (frame_count - 1));
  if (wait && reader->wait_counter == 0)
    reader->wait_counter = 1;

  reader->state = OBIS_STATE_PROCESSING;

  if (obis_count > 0 && data != NULL) {
    uint16_t idx = 0;
    uint8_t processed_obis = 0;

    while ((idx + 2 <= data_len) && (processed_obis < obis_count)) {
      uint8_t length = data[idx++];
      uint8_t index = data[idx++];
      processed_obis++;

      obis_code_t *target = NULL;
      for (int c = 0; c < reader->info.active_code_count; c++) {
        // --- GÜNCELLENDİ: Statik 'index' yerine dinamik 'temp_index'
        // kullanılıyor --- Sadece henüz alınmamış ve pakette giden dinamik
        // indeksi eşleşen OBIS'i hedef al
        if (!reader->info.codes[c].is_received &&
            reader->info.codes[c].temp_index == index) {
          target = &reader->info.codes[c];
          break;
        }
      }

      if (target == NULL || frame_id >= OBIS_MAX_FRAMES) {
        idx += length;
        continue;
      }

      target->frame_count = frame_count;
      uint16_t copy_len = (data_len - idx < length) ? (data_len - idx) : length;
      if (copy_len > (OBIS_VALUE_MAX_LEN - 1))
        copy_len = OBIS_VALUE_MAX_LEN - 1;

      memcpy(target->frame_buffers[frame_id], &data[idx], copy_len);
      target->frame_buffers[frame_id][copy_len] = '\0';
      target->frame_lens[frame_id] = copy_len;
      target->received_frame_mask |= (1 << frame_id);

      // Frame ID sensorden bozuk gelebildigi icin bu maske her zaman dogru
      // calismayabilir. Ama buffer kaydini basariyla tamamliyoruz.
      int pos = 0;
      for (int f = 0; f <= frame_id; f++) {
        uint16_t len_to_copy = target->frame_lens[f];
        if (len_to_copy > 0 && pos + len_to_copy < OBIS_VALUE_MAX_LEN) {
          memcpy(&target->value_buf[pos], target->frame_buffers[f],
                 len_to_copy);
          pos += len_to_copy;
        }
      }
      target->value_buf[pos] = '\0';
      target->is_received = true;

      db_save_obis_read(reader->info.meter_serial, target->code,
                        target->value_buf);

      idx += length;
    }

    bool all_received = true;
    for (int c = 0; c < reader->info.active_code_count; c++) {
      if (!reader->info.codes[c].is_received) {
        all_received = false;
        break;
      }
    }

    if (all_received) {
      reader->state = OBIS_STATE_SUCCEED;
      ESP_LOGI(TAG, ">>> TUM OBIS VERILERI BASARIYLA ALINDI! (Metre: %s) <<<",
               reader->info.meter_serial);
    } else if (!wait) {
      // Tüm veriler henüz bitmedi. Kalanlar için FSM'yi tekrar istek moduna
      // sok!
      reader->state = OBIS_STATE_REQUESTING;
      reader->send_req_counter =
          reader->send_req_limit; // Timer beklemeden bir sonraki cycle'da hemen
                                  // yollasın
      ESP_LOGI(TAG, ">>> Kismi okuma tamamlandi. Kalan OBIS'ler icin yeni "
                    "paket (Chunk) hazirlaniyor...");
    }
  } else {
    reader->state = OBIS_STATE_FAILED;
  }

  xSemaphoreGive(obis_pool_mutex);
}

esp_err_t obis_manager_add_test_node(const char *node_serial,
                                     const struct sockaddr_in6 *node_addr,
                                     int read_period, const char *meter_serial,
                                     uint8_t start_baud_rate,
                                     const char *obis_codes[], int obis_count) {
  if (xSemaphoreTake(obis_pool_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  int slot = -1;
  for (int i = 0; i < OBIS_MAX_NODES; i++) {
    if (!obis_pool[i].is_active) {
      slot = i;
      break;
    }
  }
  if (slot == -1) {
    xSemaphoreGive(obis_pool_mutex);
    return ESP_ERR_NO_MEM;
  }

  obis_reader_t *reader = &obis_pool[slot];
  memset(reader, 0, sizeof(obis_reader_t));

  strncpy(reader->node_serial, node_serial, SERIAL_MAX_LEN - 1);
  memcpy(&reader->node_addr, node_addr, sizeof(struct sockaddr_in6));
  reader->read_period = read_period;
  reader->remaining_time_sec = read_period;
  reader->seq_number = (esp_random() % 31) + 1;
  reader->state = OBIS_STATE_REQUESTING;
  reader->send_req_counter = 0;
  reader->send_req_limit = 1;

  strncpy(reader->info.meter_serial, meter_serial, SERIAL_MAX_LEN - 1);
  reader->info.start_baud_rate = start_baud_rate;

  int code_limit = (obis_count < OBIS_MAX_CODES_PER_READER)
                       ? obis_count
                       : OBIS_MAX_CODES_PER_READER;

  // --- GÜNCELLENDİ: Statik harita çöpe atıldı, doğrudan metinler kaydediliyor
  // ---
  for (int i = 0; i < code_limit; i++) {
    reader->info.codes[i].temp_index =
        0; // Başlangıçta 0 atanır, gönderirken dinamik verilir
    strncpy(reader->info.codes[i].code, obis_codes[i], OBIS_CODE_MAX_LEN - 1);
    reader->info.codes[i].is_received = false;
  }

  reader->info.active_code_count = code_limit;
  reader->is_active = true;

  xSemaphoreGive(obis_pool_mutex);
  ESP_LOGI(TAG, "Test Node eklendi -> Node: %s | Meter: %s", node_serial,
           meter_serial);
  return ESP_OK;
}

void obis_manager_init(int sock) {
  udp_sock = sock;
  memset(obis_pool, 0, sizeof(obis_pool));
  obis_pool_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(obis_pool_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
  ESP_LOGI(TAG, "OBIS Manager Ilklendi. Socket ID: %d", udp_sock);
}
void obis_set_state_by_addr(const struct sockaddr_in6 *src_addr,
                            obis_state_t new_state) {
  // EĞER OBIS MANAGER BAŞLATILMADIYSA (MUTEX NULL İSE) İŞLEM YAPMA VE ÇIK!
  if (obis_pool_mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(obis_pool_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < OBIS_MAX_NODES; i++) {
      if (obis_pool[i].is_active &&
          memcmp(&obis_pool[i].node_addr.sin6_addr, &src_addr->sin6_addr,
                 sizeof(struct in6_addr)) == 0) {

        obis_pool[i].state = new_state;
        ESP_LOGW(TAG,
                 "OR: Node %s - Metreden hata uyarisi alindi. FSM WAITING "
                 "moduna gecirildi.",
                 obis_pool[i].node_serial);
        break;
      }
    }
    xSemaphoreGive(obis_pool_mutex);
  }
}

void obis_notify_node_online(const struct sockaddr_in6 *src_addr) {
  if (obis_pool_mutex == NULL || src_addr == NULL)
    return;

  if (xSemaphoreTake(obis_pool_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < OBIS_MAX_NODES; i++) {
      if (obis_pool[i].is_active &&
          memcmp(&obis_pool[i].node_addr.sin6_addr, &src_addr->sin6_addr,
                 sizeof(struct in6_addr)) == 0) {

        if (obis_pool[i].state == OBIS_STATE_REQUESTING ||
            obis_pool[i].state == OBIS_STATE_WAITING) {
          obis_pool[i].send_req_limit = 1;
          obis_pool[i].send_req_counter = 1;
          obis_pool[i].state = OBIS_STATE_REQUESTING;
        }
        break;
      }
    }
    xSemaphoreGive(obis_pool_mutex);
  }
}

// yükprofili
void process_load_read_request(const uint8_t *data, uint16_t len,
                               struct sockaddr_in6 *source_addr) {
  // A + B + C + D(en az 1) = Minimum 4 byte olmalı
  if (len < 4) {
    ESP_LOGE(TAG, "Gecersiz/Kisa Yuk Profili Paketi! (Len: %d)", len);
    return;
  }

  // A: Paket Tipi Doğrulama
  if (data[0] != MR_LOAD_REQUEST) {
    ESP_LOGE(TAG, "Bilinmeyen paket tipi: 0x%02X (Beklenen: 0x0C)", data[0]);
    return;
  }

  load_profile_request_t req = {0};

  uint8_t serial_len_and_flag = data[1];
  req.last_succeed = (serial_len_and_flag & 0x01) == 1; // Sadece 1. bit (LSB)
  uint8_t serial_len = (serial_len_and_flag >> 2) &
                       0x1F; // Sadece 3,4,5,6,7. bitler (0x1F Maskesi)

  req.seq_number = data[2];

  int offset = 3;
  if (serial_len >= sizeof(req.meter_serial) || (offset + serial_len) > len) {
    ESP_LOGE(TAG, "Seri No uzunlugu hatali veya paketten tasiyor!");
    return;
  }
  memcpy(req.meter_serial, &data[offset], serial_len);
  req.meter_serial[serial_len] = '\0'; // Güvenli string sonlandırma
  offset += serial_len;

  // E: Date-String Uzunluğu
  if (offset >= len) {
    ESP_LOGW(TAG, "Paket erken bitti, date-string parametresi yok.");
    return;
  }
  req.date_string_len = data[offset++];

  if (req.date_string_len > 0) {
    if (offset + req.date_string_len > len) {
      ESP_LOGE(TAG, "Date string paketten tasiyor!");
      return;
    }

    // +1 ekleyerek Null Terminator için yer ayırıyoruz
    req.date_string = (char *)malloc(req.date_string_len + 1);
    if (req.date_string == NULL) {
      ESP_LOGE(TAG, "RAM yetersiz, Date String icin alan ayrilamadi!");
      return;
    }

    memcpy(req.date_string, &data[offset], req.date_string_len);
    req.date_string[req.date_string_len] = '\0';
    offset += req.date_string_len;
  } else {
    req.date_string = NULL;
  }

  if (offset < len) {
    req.baud_rate_indicator = data[offset];
  } else {
    req.baud_rate_indicator = 0x00;
  }

  ESP_LOGI(TAG, "--- YUK PROFILI ISTEGI (LOAD REQUEST: 0x0C) ---");
  ESP_LOGI(TAG, "Meter: %s | Seq: %d | LastSucceed: %d", req.meter_serial,
           req.seq_number, req.last_succeed);
  ESP_LOGI(TAG, "Baud Ind: 0x%02X", req.baud_rate_indicator);

  if (req.date_string != NULL) {
    ESP_LOGI(TAG, "Date String (%d bytes): %s", req.date_string_len,
             req.date_string);
  } else {
    ESP_LOGI(TAG, "Date String: Yok (Standart Okuma Yapilabilir)");
  }

  // TODO: Burada `req.date_string` içerisindeki tarih formatını (örn: P.01)
  // sayacın anlayacağı IEC komutuna dönüştürüp, ilgili obis_manager FSM'sine
  // veya UART kuyruğuna atmalısın.

  if (req.date_string != NULL) {
    free(req.date_string);
  }
}

// --- PERİYODİK OBİS YÖNETİM FONKSİYONLARI ---

bool obis_mgr_add_periodic_task(const char *meter_serial, uint8_t baud,
                                uint8_t interval, uint8_t total_obis,
                                uint16_t total_len, uint8_t seq,
                                uint8_t pkt_count) {
  int slot = -1;

  // 1. Önce bu sayaç zaten var mı kontrol et
  for (int i = 0; i < MAX_PERIODIC_METERS; i++) {
    if (periodic_tasks[i].is_active &&
        strcmp(periodic_tasks[i].meter_serial, meter_serial) == 0) {
      slot = i;
      break;
    }
  }

  // 2. Yoksa boş bir yer bul
  if (slot == -1) {
    for (int i = 0; i < MAX_PERIODIC_METERS; i++) {
      if (!periodic_tasks[i].is_active) {
        slot = i;
        break;
      }
    }
  }

  // 3. Yer bulunduysa kaydet
  if (slot != -1) {
    // Eski RAM tahsislerini temizle (Memory Leak Koruması)
    if (periodic_tasks[slot].obis_codes != NULL) {
      for (int i = 0; i < periodic_tasks[slot].total_obis_count; i++) {
        if (periodic_tasks[slot].obis_codes[i] != NULL) {
          free(periodic_tasks[slot].obis_codes[i]);
        }
      }
      free(periodic_tasks[slot].obis_codes);
    }

    periodic_tasks[slot].is_active = true;
    strcpy(periodic_tasks[slot].meter_serial, meter_serial);
    periodic_tasks[slot].baud_rate_indicator = baud;
    periodic_tasks[slot].interval = interval;
    periodic_tasks[slot].total_obis_count = total_obis;
    periodic_tasks[slot].total_length = total_len;
    periodic_tasks[slot].seq_num = seq;
    periodic_tasks[slot].expected_packet_count = pkt_count;
    periodic_tasks[slot].stored_obis_count = 0;
    periodic_tasks[slot].last_read_timestamp = 0;

    // OBIS kodları için pointer dizisini oluştur
    periodic_tasks[slot].obis_codes =
        (char **)calloc(total_obis, sizeof(char *));

    ESP_LOGI("OBIS_MGR", "Periyodik Gorev Olusturuldu: %s", meter_serial);
    return true;
  }

  ESP_LOGE("OBIS_MGR", "Periyodik Gorev Listesi Dolu!");
  return false;
}

bool obis_mgr_add_periodic_obis(const char *meter_serial,
                                const char *obis_code) {
  for (int i = 0; i < MAX_PERIODIC_METERS; i++) {
    if (periodic_tasks[i].is_active &&
        strcmp(periodic_tasks[i].meter_serial, meter_serial) == 0) {
      if (periodic_tasks[i].stored_obis_count <
          periodic_tasks[i].total_obis_count) {
        // OBIS stringini kopyala ve listeye ekle
        periodic_tasks[i].obis_codes[periodic_tasks[i].stored_obis_count] =
            strdup(obis_code);
        periodic_tasks[i].stored_obis_count++;
        return true;
      }
      break;
    }
  }
  return false;
}

void obis_mgr_delete_periodic_task(const char *meter_serial) {
  for (int i = 0; i < MAX_PERIODIC_METERS; i++) {
    if (periodic_tasks[i].is_active &&
        strcmp(periodic_tasks[i].meter_serial, meter_serial) == 0) {
      for (int j = 0; j < periodic_tasks[i].total_obis_count; j++) {
        if (periodic_tasks[i].obis_codes[j]) {
          free(periodic_tasks[i].obis_codes[j]);
        }
      }
      free(periodic_tasks[i].obis_codes);
      periodic_tasks[i].is_active = false;
      ESP_LOGI("OBIS_MGR", "Periyodik Gorev Silindi: %s", meter_serial);
      break;
    }
  }
}