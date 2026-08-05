#ifndef METER_DB_H
#define METER_DB_H

#include <sqlite3.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
// Kuyruk mesaj yapısı
typedef struct {
    char meterSerial[16];
    uint8_t *data;
    int dataLen;
    bool isLongRead;
    bool isCompressed;
    bool is_delta;
} db_write_request_t;

// Scheduler için Okuma İstek Parametreleri Yapısı
typedef struct {
    char nodeSerial[16];
    char meterSerial[16];
    struct sockaddr_in6 destAddr;
    int startBaudRate;
    bool hasXorData;
    uint8_t xorVal;
    uint16_t lastReadoutLen;
    bool isLongRead;
} meter_read_target_t;

// Global kuyruk (main.c'de tanımlanır, burada extern)
extern QueueHandle_t sensor_queue;

// Fonksiyon prototipleri
void init_vfs(void);
void init_database(void);
void sqlite_task(void *pvParameters);
void save_meter_read_to_db(const char *meterSerial, const uint8_t *data, int dataLen, bool isLongRead, bool isCompressed, const char *error_msg);
void mark_meter_request_sent(const char *meterSerial);
// Scheduler Yardımcı Fonksiyonları (Thread-safe DB sorguları)
bool get_next_meter_to_read(meter_read_target_t *target);
void mark_meter_last_access(const char *meterSerial, bool status,int error_code);
bool update_meter_read_status(const char *meterSerial);
void log_payload_tail(const uint8_t *payload, size_t len);
#endif // METER_DB_H