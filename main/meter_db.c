#include "meter_db.h"
#include "meter_protocol.h"   // generate_ulid için
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"       // esp_fill_random için
#include <string.h>
#include <stdlib.h>
#include "esp_littlefs.h"
#include <sys/unistd.h>
#include <sys/stat.h>

static const char *TAG = "METER_DB";
static sqlite3 *db = NULL;

// sensor_queue sadece main.c'de tanımlanır, burada extern ile bildirilir
extern QueueHandle_t sensor_queue;

// ------------------------------------------------------------------
// SPIFFS Başlatma
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
        ESP_LOGI(TAG, "LittleFS Total: %d KB, Used: %d KB", total/1024, used/1024);
    }
}

// ------------------------------------------------------------------
// SQLite Başlatma (ÖNEMLİ: sqlite3_initialize çağrısı eklendi!)
// ------------------------------------------------------------------
void init_database(void) {
    int rc;
    char *err_msg = 0;

    sqlite3_initialize();

    // LittleFS kullandığımız için dosya yolu /storage/ ile başlıyor.
    // nolock=1 VFS kilitlenmelerini, uri=1 bu parametrelerin okunmasını sağlar.
    rc = sqlite3_open_v2("file:/storage/meter_data.db?nolock=1", &db, 
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot open database: %s", sqlite3_errmsg(db));
        return;
    }

    // Flash belleği fiziksel olarak korumak ve I/O hızını artırmak için PRAGMA'lar
    ESP_ERROR_CHECK_WITHOUT_ABORT(sqlite3_exec(db, "PRAGMA journal_mode=MEMORY;", 0, 0, &err_msg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(sqlite3_exec(db, "PRAGMA synchronous=OFF;", 0, 0, &err_msg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(sqlite3_exec(db, "PRAGMA temp_store=MEMORY;", 0, 0, &err_msg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(sqlite3_exec(db, "PRAGMA page_size=4096;", 0, 0, &err_msg)); // Flash sektör boyutuyla hizala

    if (err_msg) {
        sqlite3_free(err_msg);
    }

    // Tablo oluşturma sorguları... (Önceki Prepared Statement yapısıyla devam)
    const char *sql_create = 
        "CREATE TABLE IF NOT EXISTS meter_reads ("
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
        "uncompressed_read_at TIMESTAMP"
        ");";

    rc = sqlite3_exec(db, sql_create, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQL Table creation error: %s", err_msg);
        sqlite3_free(err_msg);
    } else {
        ESP_LOGI(TAG, "Database initialized successfully in LittleFS");
    }
}

// ------------------------------------------------------------------
// SQLite Arka Plan Görevi (kuyruktan veri alır ve kaydeder)
// ------------------------------------------------------------------
void sqlite_task(void *pvParameters) {
    db_write_request_t req;
    while (1) {
        if (xQueueReceive(sensor_queue, &req, portMAX_DELAY) == pdTRUE) {
            save_meter_read_to_db(req.meterSerial, req.data, req.dataLen, req.isLongRead, req.isCompressed);
            free(req.data);
        }
    }
}

// ------------------------------------------------------------------
// Veritabanına Kayıt
// ------------------------------------------------------------------
void save_meter_read_to_db(const char *meterSerial, const uint8_t *data, int dataLen, bool isLongRead, bool isCompressed) {
    if (!db) {
        ESP_LOGE(TAG, "DB handle is NULL");
        return;
    }

    char id[27];
    generate_ulid(id);

    uint8_t xor_val = 0;
    for (int i = 0; i < dataLen; i++) xor_val ^= data[i];

    // ==========================================
    // 1. INSERT İşlemi
    // ==========================================
    const char *insert_sql = "INSERT OR REPLACE INTO meter_reads (id, meter_serial, is_long_read, data, length, xor, errors) VALUES (?, ?, ?, ?, ?, ?, NULL);";
    sqlite3_stmt *insert_stmt = NULL;
    
    int rc = sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "INSERT prepare failed: %s", sqlite3_errmsg(db));
        return;
    }

    // SQLITE_STATIC, verinin kopyalanmasına gerek olmadığını, fonksiyon stack'inde veya dışarıdan gelen pointer'da (meterSerial) güvenli olduğunu belirtir.
    sqlite3_bind_text(insert_stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(insert_stmt, 2, meterSerial, -1, SQLITE_STATIC);
    sqlite3_bind_int(insert_stmt,  3, isLongRead ? 1 : 0);
    sqlite3_bind_blob(insert_stmt, 4, data, dataLen, SQLITE_STATIC);
    sqlite3_bind_int(insert_stmt,  5, dataLen);
    sqlite3_bind_int(insert_stmt,  6, xor_val);

    rc = sqlite3_step(insert_stmt);
    if (rc != SQLITE_DONE) {
        ESP_LOGE(TAG, "INSERT step failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(insert_stmt);

    // ==========================================
    // 2. UPDATE İşlemi (Hatalı snprintf bloğunun düzeltilmiş hali)
    // ==========================================
    const char *update_sql = "UPDATE meterInfo SET meterLastAccess=FALSE, uncompressed_read_at=CURRENT_TIMESTAMP WHERE meterSerial=?;";
    sqlite3_stmt *update_stmt = NULL;

    rc = sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, NULL);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "UPDATE prepare failed: %s", sqlite3_errmsg(db));
        return;
    }

    // Pointer formatlamasına girmeden doğrudan binding yapıyoruz
    sqlite3_bind_text(update_stmt, 1, meterSerial, -1, SQLITE_STATIC);

    rc = sqlite3_step(update_stmt);
    if (rc != SQLITE_DONE) {
        ESP_LOGE(TAG, "UPDATE step failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(update_stmt);

    ESP_LOGI(TAG, "Saved meter read for %s, length %d", meterSerial, dataLen);
}