#include "meter_db.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "meter_protocol.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "METER_DB";
static sqlite3 *db = NULL;

extern QueueHandle_t sensor_queue;
// SON 10 BYTE GÖREBİLME MEVZUSU
void log_payload_tail(const uint8_t *payload, size_t len) {
  if (payload == NULL || len == 0)
    return;

  if (len < 10) {
    ESP_LOG_BUFFER_HEX("METER_RX", payload, len);
    return;
  }
  const uint8_t *tail_ptr = payload + (len - 10);
  ESP_LOGI("METER_RX", "--- Payload Son 10 Byte (Hex & ASCII) ---");
  ESP_LOG_BUFFER_HEX("METER_RX", tail_ptr, 10);
  ESP_LOGI("METER_RX", "ASCII: %.*s", 10, tail_ptr);
}
// ------------------------------------------------------------------
// LittleFS VFS Başlatma
// ------------------------------------------------------------------
void init_vfs(void) {
  esp_vfs_littlefs_conf_t conf = {
      .base_path = "/storage",
      .partition_label = "storage",
      .format_if_mount_failed = true,
      .dont_mount = false,
  };
  esp_err_t ret = esp_vfs_littlefs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LittleFS mount failed (%s)", esp_err_to_name(ret));
    return;
  }
  size_t total = 0, used = 0;
  ret = esp_littlefs_info(conf.partition_label, &total, &used);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get LittleFS info (%s)", esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "LittleFS Total: %d KB, Used: %d KB", (int)(total / 1024),
             (int)(used / 1024));
  }
}
static int get_user_version(sqlite3 *db) {
  sqlite3_stmt *stmt;
  int version = 0;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) ==
      SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  return version;
}

static void set_user_version(sqlite3 *db, int version) {
  char sql[64];
  snprintf(sql, sizeof(sql), "PRAGMA user_version = %d;", version);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
}

sqlite3 *get_db_handle(void) { return db; }
// ------------------------------------------------------------------
// SQLite Başlatma ve Tablo Oluşturma
// ------------------------------------------------------------------
void init_database(void) {
  int rc;
  char *err_msg = 0;

  sqlite3_initialize();
  rc = sqlite3_open_v2(
      "file:/storage/meter_data.db?nolock=1", &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
  if (rc != SQLITE_OK) {
    ESP_LOGE(TAG, "Cannot open database: %s", sqlite3_errmsg(db));
    return;
  }

  // Optimizasyon PRAGMA'ları
  sqlite3_exec(db, "PRAGMA journal_mode=OFF;", 0, 0, &err_msg);
  sqlite3_exec(db, "PRAGMA synchronous=OFF;", 0, 0, &err_msg);
  sqlite3_exec(db, "PRAGMA temp_store=FILE;", 0, 0, &err_msg);
  sqlite3_exec(db, "PRAGMA page_size=512;", 0, 0, &err_msg);
  sqlite3_exec(db, "PRAGMA cache_size=10;", 0, 0, &err_msg);
  if (err_msg) {
    sqlite3_free(err_msg);
    err_msg = NULL;
  }

  int current_version = get_user_version(db);
  ESP_LOGI(TAG, "Mevcut Veritabani Versiyonu: %d", current_version);

  // Versiyon 0 ise (İlk kurulum)
  if (current_version < 1) {
    ESP_LOGI(TAG, "Veritabani guncelleniyor (Temel Tablolar)...");
    const char *sql_v1 = "CREATE TABLE IF NOT EXISTS meter_reads ("
                         "id CHAR(26) PRIMARY KEY,"
                         "meter_serial VARCHAR(12) NOT NULL,"
                         "is_long_read BOOLEAN DEFAULT FALSE,"
                         "data BLOB NOT NULL,"
                         "length SMALLINT NOT NULL,"
                         "xor TINYINT NOT NULL,"
                         "errors TEXT,"
                         "read_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
                         ");"
                         "CREATE TABLE IF NOT EXISTS meterInfo ("
                         "meterSerial VARCHAR(12) PRIMARY KEY,"
                         "nodeSerial VARCHAR(16) NOT NULL,"
                         "startBaudRate INTEGER NOT NULL,"
                         "meterLastAccess BOOLEAN DEFAULT TRUE,"
                         "is_long_read BOOLEAN DEFAULT FALSE, "
                         "uncompressed_read_at TIMESTAMP,"
                         "long_read_completed_at TIMESTAMP, "
                         "last_len SMALLINT DEFAULT 0,"
                         "last_xor TINYINT DEFAULT 0,"
                         "retry_count INTEGER DEFAULT 0,"
                         "last_error_code INTEGER DEFAULT 0"
                         ");"
                         "CREATE TABLE IF NOT EXISTS nodeInfo ("
                         "nodeSerial VARCHAR(16) PRIMARY KEY,"
                         "nodeIp BLOB NOT NULL,"
                         "nodeReadPeriod INTEGER NOT NULL,"
                         "synced_at TIMESTAMP"
                         ");";

    rc = sqlite3_exec(db, sql_v1, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
      ESP_LOGE(TAG, "V1 Migration Hatasi: %s", err_msg);
      sqlite3_free(err_msg);
    } else {
      set_user_version(db, 1);
      current_version = 1;
    }
  }

  ESP_LOGI(TAG, "Veritabani hazir. Aktif Versiyon: %d", current_version);

  sqlite3_exec(db,
               "INSERT OR REPLACE INTO nodeInfo (nodeSerial, nodeIp, "
               "nodeReadPeriod, synced_at) "
               "VALUES ('NODE_71A1', 'fd00::212:4b00:3486:71a1', 60, "
               "CURRENT_TIMESTAMP);",
               0, 0, &err_msg);
  if (err_msg) {
    sqlite3_free(err_msg);
    err_msg = NULL;
  }
  /*
      sqlite3_exec(db,
          "INSERT OR REPLACE INTO meterInfo (meterSerial, nodeSerial,
     startBaudRate, meterLastAccess, is_long_read) " "VALUES ('MSY22422640',
     'NODE_71A1', 300, 1, 1);", 0, 0, &err_msg); if (err_msg) {
     sqlite3_free(err_msg); err_msg = NULL; }*/
  sqlite3_exec(db,
               "INSERT OR REPLACE INTO meterInfo (meterSerial, nodeSerial, "
               "startBaudRate, meterLastAccess, is_long_read) "
               "VALUES ('BYL340001360', 'NODE_71A1', 9600, 1, 1);",
               0, 0, &err_msg);
  if (err_msg) {
    sqlite3_free(err_msg);
    err_msg = NULL;
  }
}

// ------------------------------------------------------------------
// Scheduler İçin Sıradaki Okunacak Sayacı ve Node IP'sini Getirir
// ------------------------------------------------------------------
bool get_next_meter_to_read(meter_read_target_t *target) {
  if (!db || !target)
    return false;

  // SQL Operatör önceliği düzeltildi: Parantezler eklendi
  const char *sql = "SELECT m.meterSerial, m.nodeSerial, m.startBaudRate, "
                    "n.nodeIp, m.is_long_read, "
                    "       m.last_xor, m.last_len "
                    "FROM meterInfo m "
                    "JOIN nodeInfo n ON m.nodeSerial = n.nodeSerial "
                    "WHERE (m.meterLastAccess = 1 "
                    "   OR m.uncompressed_read_at IS NULL "
                    "   OR (strftime('%s', 'now') - strftime('%s', "
                    "m.uncompressed_read_at)) >= n.nodeReadPeriod) "
                    "  AND m.retry_count < 3 "
                    "LIMIT 1;";

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    ESP_LOGE(TAG, "SQL Prepare failed: %s", sqlite3_errmsg(db));
    return false;
  }

  bool found = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    memset(target, 0, sizeof(meter_read_target_t));

    const unsigned char *mSerial = sqlite3_column_text(stmt, 0);
    const unsigned char *nSerial = sqlite3_column_text(stmt, 1);

    if (!mSerial || !nSerial || mSerial[0] == '\0') {
      sqlite3_finalize(stmt);
      return false;
    }

    snprintf(target->meterSerial, sizeof(target->meterSerial), "%s", mSerial);
    snprintf(target->nodeSerial, sizeof(target->nodeSerial), "%s", nSerial);

    int baud = sqlite3_column_int(stmt, 2);
    target->startBaudRate = (baud == 0) ? 300 : baud;
    target->isLongRead = (sqlite3_column_int(stmt, 4) != 0);

    int lastXor = sqlite3_column_int(stmt, 5);
    int lastLen = sqlite3_column_int(stmt, 6);

    if (lastLen > 0) {
      target->hasXorData = true;
      target->xorVal = (uint8_t)lastXor;
      target->lastReadoutLen = (uint16_t)lastLen;
    } else {
      target->hasXorData = false;
      target->xorVal = 0;
      target->lastReadoutLen = 0;
    }

    target->destAddr.sin6_family = AF_INET6;
    target->destAddr.sin6_port = htons(8008);

    int ip_type = sqlite3_column_type(stmt, 3);
    if (ip_type == SQLITE_BLOB && sqlite3_column_bytes(stmt, 3) == 16) {
      memcpy(&target->destAddr.sin6_addr, sqlite3_column_blob(stmt, 3), 16);
      found = true;
    } else if (ip_type == SQLITE_TEXT) {
      if (inet_pton(AF_INET6, (const char *)sqlite3_column_text(stmt, 3),
                    &target->destAddr.sin6_addr) == 1) {
        found = true;
      }
    }
  }

  sqlite3_finalize(stmt);
  return found;
}

void mark_meter_request_sent(const char *meterSerial) {
  if (!db || !meterSerial)
    return;
  const char *sql =
      "UPDATE meterInfo SET meterLastAccess = 0 WHERE meterSerial = ?;";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, meterSerial, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}
// ------------------------------------------------------------------
// Sayacın Erişim Durumunu Günceller (Erişildi / Bekliyor)
// ------------------------------------------------------------------
void mark_meter_last_access(const char *meterSerial, bool status,
                            int error_code) {
  if (!db || !meterSerial)
    return;

  const char *sql =
      "UPDATE meterInfo SET "
      "meterLastAccess = ?, "
      "retry_count = CASE WHEN ? THEN 0 ELSE retry_count + 1 END, "
      "last_error_code = ? "
      "WHERE meterSerial = ?;";

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, status ? 1 : 0);
    sqlite3_bind_int(stmt, 2, status ? 1 : 0);
    sqlite3_bind_int(stmt, 3, error_code);
    sqlite3_bind_text(stmt, 4, meterSerial, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

// ------------------------------------------------------------------
// SQLite Arka Plan Görüntüleme Task'ı (Kuyruktan Okuyup Kaydeder)
// ------------------------------------------------------------------
#include "esp_heap_caps.h"

// 32-bit hizalamalı, Xtensa mimarisi için optimize edilmiş XOR
static void apply_xor_delta_32(uint8_t *target, const uint8_t *delta,
                               size_t len) {
  size_t words = len / 4;
  uint32_t *t32 = (uint32_t *)target;
  const uint32_t *d32 = (const uint32_t *)delta;

  for (size_t w = 0; w < words; w++) {
    t32[w] ^= d32[w];
  }
  for (size_t i = words * 4; i < len; i++) {
    target[i] ^= delta[i];
  }
}

void sqlite_task(void *pvParameters) {
  db_write_request_t req;

  while (1) {
    if (xQueueReceive(sensor_queue, &req, portMAX_DELAY) == pdTRUE) {
      uint8_t *final_data = req.data;

      if (req.is_delta) {
        const char *sql_ref = "SELECT data, length FROM meter_reads WHERE "
                              "meter_serial = ? ORDER BY read_at DESC LIMIT 1;";
        sqlite3_stmt *stmt;

        if (sqlite3_prepare_v2(db, sql_ref, -1, &stmt, NULL) == SQLITE_OK) {
          sqlite3_bind_text(stmt, 1, req.meterSerial, -1, SQLITE_STATIC);

          if (sqlite3_step(stmt) == SQLITE_ROW) {
            size_t ref_len = sqlite3_column_int(stmt, 1);
            if (ref_len == req.dataLen) {
              const uint8_t *ref_data =
                  (const uint8_t *)sqlite3_column_blob(stmt, 0);
              apply_xor_delta_32(final_data, ref_data, req.dataLen);
              ESP_LOGI(TAG, "XOR Rekonstruksiyonu tamamlandi: %d bytes",
                       req.dataLen);
            } else {
              ESP_LOGW(
                  TAG,
                  "XOR Delta uzunluk uyusmazligi! (Beklenen: %d, Gelen: %d)",
                  ref_len, req.dataLen);
            }
          } else {
            ESP_LOGW(TAG, "XOR referans kaydi bulunamadi!");
          }
          sqlite3_finalize(stmt);
        }
      }

      //  Hata mesajı parametresi
      save_meter_read_to_db(req.meterSerial, final_data, req.dataLen,
                            req.isLongRead, req.is_delta, NULL);

      heap_caps_free(req.data);
    }
  }
}

// ------------------------------------------------------------------
// Veritabanına Okuma Sonucunu Kaydeder
// ------------------------------------------------------------------
void save_meter_read_to_db(const char *meterSerial, const uint8_t *data,
                           int dataLen, bool isLongRead, bool isCompressed,
                           const char *error_msg) {
  if (!db) {
    ESP_LOGE(TAG, "DB handle is NULL");
    return;
  }

  char id[27];
  if (generate_ulid(id, sizeof(id)) != ESP_OK) {
    ESP_LOGE(TAG, "ULID oluşturulamadı!");
    return;
  }

  uint8_t xor_val = 0;
  for (int i = 0; i < dataLen; i++)
    xor_val ^= data[i];

  // 1. INSERT meter_reads (errors alanı artık error_msg ile dolduruluyor)
  const char *insert_sql =
      "INSERT OR REPLACE INTO meter_reads (id, meter_serial, is_long_read, "
      "data, length, xor, errors) VALUES (?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *insert_stmt = NULL;

  if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(insert_stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(insert_stmt, 2, meterSerial, -1, SQLITE_STATIC);
    sqlite3_bind_int(insert_stmt, 3, isLongRead ? 1 : 0);
    sqlite3_bind_blob(insert_stmt, 4, data, dataLen, SQLITE_STATIC);
    sqlite3_bind_int(insert_stmt, 5, dataLen);
    sqlite3_bind_int(insert_stmt, 6, xor_val);
    sqlite3_bind_text(insert_stmt, 7, error_msg, -1, SQLITE_STATIC);

    if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
      ESP_LOGE(TAG, "INSERT step failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(insert_stmt);
  }

  // 2. UPDATE meterInfo (Tarih damgaları ve durum güncellemesi)
  const char *update_sql = "UPDATE meterInfo SET "
                           "  meterLastAccess = 1, "
                           "  retry_count = 0, "
                           "  last_error_code = 0, "
                           "  uncompressed_read_at = CASE WHEN ? = 1 THEN "
                           "CURRENT_TIMESTAMP ELSE uncompressed_read_at END, "
                           "  long_read_completed_at = CASE WHEN ? = 1 THEN "
                           "CURRENT_TIMESTAMP ELSE long_read_completed_at END, "
                           "  last_len = ?, "
                           "  last_xor = ? "
                           "WHERE meterSerial = ?;";

  sqlite3_stmt *upd_stmt = NULL;
  if (sqlite3_prepare_v2(db, update_sql, -1, &upd_stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int(upd_stmt, 1, isCompressed ? 0 : 1);
    sqlite3_bind_int(upd_stmt, 2, isLongRead ? 1 : 0);
    sqlite3_bind_int(upd_stmt, 3, dataLen);
    sqlite3_bind_int(upd_stmt, 4, xor_val);
    sqlite3_bind_text(upd_stmt, 5, meterSerial, -1, SQLITE_STATIC);

    // 🔥 DÜZELTME 1: Güncelleme başarılı mı kontrol et ve logla
    if (sqlite3_step(upd_stmt) != SQLITE_DONE) {
      ESP_LOGE(TAG, "UPDATE meterInfo step failed for %s: %s", meterSerial,
               sqlite3_errmsg(db));
    } else {
      ESP_LOGI(TAG,
               "UPDATE meterInfo succeeded for %s (last_len=%d, last_xor=%d)",
               meterSerial, dataLen, xor_val);
    }
    sqlite3_finalize(upd_stmt);
  } else {
    ESP_LOGE(TAG, "UPDATE meterInfo prepare failed for %s: %s", meterSerial,
             sqlite3_errmsg(db));
  }

  ESP_LOGI(TAG, "Saved meter read for %s, length %d bytes", meterSerial,
           dataLen);
  sqlite3_db_release_memory(db);
}
bool update_meter_read_status(const char *meterSerial) {
  if (!db || !meterSerial)
    return false;

  // meterLastAccess'i 0'la ve son okuma zamanını 'şu an' olarak mühürle
  const char *sql = "UPDATE meterInfo SET "
                    "meterLastAccess = 0, "
                    "uncompressed_read_at = CURRENT_TIMESTAMP "
                    "WHERE meterSerial = ?;";

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    ESP_LOGE(TAG, "Update prepare hatasi: %s", sqlite3_errmsg(db));
    return false;
  }

  sqlite3_bind_text(stmt, 1, meterSerial, -1, SQLITE_TRANSIENT);

  bool success = false;
  if (sqlite3_step(stmt) == SQLITE_DONE) {
    success = true;
    ESP_LOGI(TAG, "Sayac (%s) periyodik zamanlayicisi sifirlandi.",
             meterSerial);
  } else {
    ESP_LOGE(TAG, "Sayac guncellenemedi: %s", sqlite3_errmsg(db));
  }

  sqlite3_finalize(stmt);
  return success;
}