#ifndef METER_PROTOCOL_H
#define METER_PROTOCOL_H
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lwip/sockets.h"
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ==========================================
// Opcode Tanımları (Java Protokolü)
// ==========================================
#define MR_READ_REQUEST     0x00
#define MR_CONFIGURE        0x01
#define MR_CONFIGURE_ACK    0x02
#define MR_CONFIGURE_NACK   0x03
#define MR_DATA             0x04
#define MR_MISS             0x05
#define MR_READ_ERROR       0x06

// ==========================================
// MR_READ_REQUEST Flags Bitmask Yapısı
// ==========================================
// Bit 0   : lastSucceed (1 = Son okuma başarılıydı)
// Bit 1   : containsXor (1 = XOR ve readoutLen paket içinde var)
// Bit 2..6: meterSerialLen (5-bit, seri numara uzunluğu)
// Bit 7   : isLongRead (1 = Uzun okuma/yük profili)
#define MR_FLAG_LAST_SUCCEED    (1 << 0)
#define MR_FLAG_CONTAINS_XOR    (1 << 1)
#define MR_FLAG_SERIAL_LEN_MASK (0x1F << 2)
#define MR_FLAG_LONG_READ       (1 << 7)

// ==========================================
// Baud Rate İndikatör Sabitleri (ASCII '0'-'6')
// ==========================================
#define METER_BAUD_300      0x30
#define METER_BAUD_600      0x31
#define METER_BAUD_1200     0x32
#define METER_BAUD_2400     0x33
#define METER_BAUD_4800     0x34
#define METER_BAUD_9600     0x35
#define METER_BAUD_19200    0x36

// ==========================================
// Zaman ve Limit Sabitleri
// ==========================================
#define MAX_COLLECT_DATA_ENTRIES 10
#define GUARD_TIME_MS            300000
#define PACKET_TIMEOUT_MS        15000
#define MAX_METER_SERIAL_LEN     16
#define MAX_READOUT_LEN          (64 * 1024) // Maksimum 64 KB okuma tahsis limiti (RAM Koruması)
// Global UDP Meter Socket (udp_meter_task içinde oluşturulur)
extern int meter_sock;

// ==========================================
// CollectData Yapısı (Paket Birleştirme Motoru)
// ==========================================
typedef struct {
    char nodeSerial[16];
    char meterSerial[MAX_METER_SERIAL_LEN];
    uint8_t sequenceNumber;
    bool receivingXorData;
    bool isLongRead;
    uint16_t readOutLen;
    uint8_t totalPacketNum;
    uint8_t dataBlockLen;
    struct sockaddr_in6 srcAddr;
    uint8_t *dataBuffer;
    uint8_t *byteMap;
    int receivedPackets;
    bool completed;
    bool failed;
    bool finalAckSent;
    TickType_t startTime;
    TickType_t guardTime;
    TickType_t packetTimeout;
    TickType_t lastPacketTime;
} collect_data_t;

// ==========================================
// Fonksiyon Prototipleri
// ==========================================




// Doğru imza: Uzunluk kesinlikle unsigned (size_t) olmalı.
void handle_meter_read(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr);

void check_all_collect_data(void);
void process_completed_data(collect_data_t *cd);

// Yardımcı Dönüşüm Fonksiyonları
uint8_t convert_baud_to_byte(int baud);
int convert_byte_to_baud(uint8_t indicator);
esp_err_t generate_ulid(char *out, size_t out_sz);

#endif // METER_PROTOCOL_H