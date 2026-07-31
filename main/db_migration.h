#ifndef DB_MIGRATION_H
#define DB_MIGRATION_H

#include "esp_err.h"
#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sunucuya baglanir, mevcut versiyonu kontrol eder, 
 *        gerekli migration json dosyasini LittleFS'e indirir ve SQLite uzerinde isletir.
 * 
 * @param db Aktif SQLite veritabani isaretcisi (handle)
 * @return esp_err_t ESP_OK ise guncelleme basarili veya gerek yok. 
 */
esp_err_t ota_db_migration_run(sqlite3 *db);

#ifdef __cplusplus
}
#endif

#endif // 