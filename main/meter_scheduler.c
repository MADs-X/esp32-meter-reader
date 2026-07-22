#include "meter_scheduler.h"
#include "meter_db.h"
#include "meter_protocol.h"
#include "esp_log.h"
#include <string.h>
#include <sqlite3.h>

static const char *TAG = "METER_SCHED";

#define SCHEDULER_PERIOD_MS 60000
static uint16_t seq_counter = 1;   // uint8_t → uint16_t

static void send_meter_read_request(const char *meterSerial, int baudRate, const struct sockaddr_in6 *dest) {
    // MR_READ_REQUEST oluştur ve gönder
    uint8_t packet[32];
    int offset = 0;
    packet[offset++] = MR_READ_REQUEST;
    uint8_t flags = 0;
    uint8_t serialLen = strlen(meterSerial);
    flags |= (serialLen & 0x1F) << 2; // bit2-6
    // bit0 lastSucceed, bit1 containsXor, bit7 longRead hep 0 (temel senaryo)
    packet[offset++] = flags;
    packet[offset++] = seq_counter++;
    if (seq_counter > 255) seq_counter = 1;

    for (int i = 0; i < serialLen; i++) packet[offset++] = meterSerial[i];

    // Baud indicator (örnek 9600 -> 0x35)
    uint8_t baud_ind;
    switch (baudRate) {
        case 300:  baud_ind = 0x30; break;
        case 600:  baud_ind = 0x31; break;
        case 1200: baud_ind = 0x32; break;
        case 2400: baud_ind = 0x33; break;
        case 4800: baud_ind = 0x34; break;
        case 9600: baud_ind = 0x35; break;
        case 19200: baud_ind = 0x36; break;
        default:   baud_ind = 0x35;
    }
    packet[offset++] = baud_ind;

    sendto(meter_sock, packet, offset, 0, (struct sockaddr *)dest, sizeof(struct sockaddr_in6));
    ESP_LOGI(TAG, "Sent MR_READ_REQUEST to %s", meterSerial);
}

void meter_scheduler_task(void *pvParameters) {
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SCHEDULER_PERIOD_MS));

        // SQLite'den nodeInfo ve meterInfo oku
        sqlite3 *db = NULL;
        sqlite3_open("/spiffs/meter_data.db", &db);
        if (!db) {
            ESP_LOGE(TAG, "Cannot open DB for scheduling");
            continue;
        }

        const char *sql = "SELECT nodeSerial, nodeIp FROM nodeInfo;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            continue;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *nodeSerial = sqlite3_column_text(stmt, 0);
            const void *ipBlob = sqlite3_column_blob(stmt, 1);
            struct sockaddr_in6 dest;
            memcpy(&dest.sin6_addr, ipBlob, 16);
            dest.sin6_family = AF_INET6;
            dest.sin6_port = htons(8008);

            // Bu node'a ait okunacak bir meterSerial bul
            char meterSerial[16];
            int baud = 9600;
            // Basitçe meterInfo'dan al (örnek)
            char sql2[256];
            snprintf(sql2, sizeof(sql2),
                "SELECT meterSerial, startBaudRate FROM meterInfo WHERE nodeSerial='%s' AND meterLastAccess=TRUE LIMIT 1;",
                nodeSerial);
            sqlite3_stmt *stmt2;
            if (sqlite3_prepare_v2(db, sql2, -1, &stmt2, NULL) == SQLITE_OK &&
                sqlite3_step(stmt2) == SQLITE_ROW) {
                strcpy(meterSerial, (const char*)sqlite3_column_text(stmt2, 0));
                baud = sqlite3_column_int(stmt2, 1);
                send_meter_read_request(meterSerial, baud, &dest);
            }
            sqlite3_finalize(stmt2);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
}