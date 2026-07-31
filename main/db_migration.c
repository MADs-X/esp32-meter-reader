#include "db_migration.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "OTA_DB_MIG";
#define MIGRATION_FILE_PATH "/storage/migration.json"
// Sunucu API Adresi (WireGuard tunnel uzerinden)
#define OTA_API_URL "http://10.8.0.251/api/ota/db_check" 

// --- 1. Yardimci Fonksiyon: Mevcut Versiyonu Oku ---
static int get_current_user_version(sqlite3 *db) {
    sqlite3_stmt *stmt;
    int version = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return version;
}

// --- 2. Sunucudan Dosyayi LittleFS'e Indirme (Stream) ---
static esp_err_t download_migration_file(int current_version) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    char url[256];
    snprintf(url, sizeof(url), "%s?mac=%02X%02X%02X%02X%02X%02X&v=%d", 
             OTA_API_URL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], current_version);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    
    if (status_code == 204) {
        ESP_LOGI(TAG, "Guncelleme yok (Cihaz zaten guncel).");
        esp_http_client_cleanup(client);
        return ESP_OK; // Guncel
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Sunucu hatasi: %d", status_code);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *f = fopen(MIGRATION_FILE_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "LittleFS dosya acilamadi!");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // RAM'i sisirmeden Stream yontemiyle yazma (Chunking)
    char buffer[512];
    int read_len;
    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, read_len, f);
    }

    fclose(f);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Migration dosyasi LittleFS'e basariyla indirildi.");
    return ESP_OK;
}

// --- 3. Indirilen Dosyayi Isletme ve Silme ---
static esp_err_t execute_migration_file(sqlite3 *db) {
    struct stat st;
    if (stat(MIGRATION_FILE_PATH, &st) != 0) {
        return ESP_OK; // Dosya yoksa islem yapma
    }

    FILE *f = fopen(MIGRATION_FILE_PATH, "r");
    if (!f) return ESP_FAIL;

    // Dosyayi RAM'e al (cJSON tum string'i ister. Dosya boyutu < 10KB olmali)
    char *json_data = malloc(st.st_size + 1);
    if (!json_data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(json_data, 1, st.st_size, f);
    json_data[st.st_size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(json_data);
    free(json_data); // String'i hemen RAM'den at

    if (!root) {
        ESP_LOGE(TAG, "JSON ayristirma hatasi!");
        remove(MIGRATION_FILE_PATH);
        return ESP_FAIL;
    }

    cJSON *target_version_item = cJSON_GetObjectItem(root, "target_version");
    cJSON *migrations = cJSON_GetObjectItem(root, "migrations");

    if (cJSON_IsArray(migrations) && cJSON_IsNumber(target_version_item)) {
        int target_v = target_version_item->valueint;
        char *err_msg = NULL;
        bool fail = false;

        // --- GLOBAL TRANSACTION BASLAT ---
        // Dongu icindeki tum islemleri tek bir semsiye altina aliyoruz.
        sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, NULL);

        cJSON *step;
        cJSON_ArrayForEach(step, migrations) {
            cJSON *sql = cJSON_GetObjectItem(step, "sql");
            cJSON *step_v = cJSON_GetObjectItem(step, "step");

            if (cJSON_IsString(sql) && cJSON_IsNumber(step_v)) {
                ESP_LOGI(TAG, "V%d icin SQL isletiliyor...", step_v->valueint);
                
                // Adim SQL'ini islet
                if (sqlite3_exec(db, sql->valuestring, 0, 0, &err_msg) != SQLITE_OK) {
                    ESP_LOGE(TAG, "SQL Hatasi (Adim %d): %s", step_v->valueint, err_msg);
                    sqlite3_free(err_msg);
                    fail = true;
                    break; // Hatada donguyu kir, asagidaki Rollback'e git
                }

                // Basariliysa versiyonu kaydet
                // Not: user_version da transaction gunlugune (journal) dahil edilir.
                char pragma_sql[64];
                snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA user_version = %d;", step_v->valueint);
                sqlite3_exec(db, pragma_sql, 0, 0, NULL);
                
                // Watchdog'u besle
                vTaskDelay(pdMS_TO_TICKS(20)); 
            }
        }

        cJSON_Delete(root);

        // --- SONUC DEGERLENDIRMESI ---
        if (!fail) {
            // Hata yoksa tum degisiklikleri veritabanina yaz
            sqlite3_exec(db, "COMMIT;", 0, 0, NULL);
            ESP_LOGI(TAG, "Migration TAMAMLANDI. Yeni Versiyon: %d. Dosya siliniyor.", target_v);
            remove(MIGRATION_FILE_PATH);
            return ESP_OK;
        } else {
            // Hata varsa yapilan her seyi (guncellenen user_version dahil) geri al
            sqlite3_exec(db, "ROLLBACK;", 0, 0, NULL);
            ESP_LOGE(TAG, "Migration BASARISIZ. Islemler geri alindi (Rollback).");
            remove(MIGRATION_FILE_PATH);
            return ESP_FAIL;
        }
    }

    cJSON_Delete(root);
    remove(MIGRATION_FILE_PATH);
    return ESP_FAIL;
}

// --- Ana Fonksiyon ---
esp_err_t ota_db_migration_run(sqlite3 *db) {
    if (!db) return ESP_ERR_INVALID_ARG;

    int current_version = get_current_user_version(db);
    ESP_LOGI(TAG, "Mevcut DB Versiyonu: %d. Sunucu kontrol ediliyor...", current_version);

    esp_err_t err = download_migration_file(current_version);
    if (err == ESP_OK) {
        // Eger dosya indiyse calistir
        err = execute_migration_file(db);
        if (err == ESP_OK) {
            // Eger versiyon degistiyse temiz bir baslangic icin cihazi yeniden baslat
            int new_version = get_current_user_version(db);
            if (new_version > current_version) {
                ESP_LOGW(TAG, "Veritabani guncellendi. Sistem yeniden baslatiliyor...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }
    }
    return err;
}