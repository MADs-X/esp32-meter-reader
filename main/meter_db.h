#ifndef METER_DB_H
#define METER_DB_H

#include <sqlite3.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

// Kuyruk mesaj yapısı
typedef struct {
    char meterSerial[16];
    uint8_t *data;
    int dataLen;
    bool isLongRead;
    bool isCompressed;
} db_write_request_t;

// Global kuyruk (main.c'de tanımlanır, burada extern)
extern QueueHandle_t sensor_queue;

// Fonksiyon prototipleri
void init_vfs(void);    
void init_database(void);
void sqlite_task(void *pvParameters);
void save_meter_read_to_db(const char *meterSerial, const uint8_t *data, int dataLen, bool isLongRead, bool isCompressed);

#endif