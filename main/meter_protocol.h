#ifndef METER_PROTOCOL_H
#define METER_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// TickType_t için FreeRTOS include
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Opcode'lar
#define MR_READ_REQUEST     0x00
#define MR_CONFIGURE        0x01
#define MR_CONFIGURE_ACK    0x02
#define MR_CONFIGURE_NACK   0x03
#define MR_DATA             0x04
#define MR_MISS             0x05
#define MR_READ_ERROR       0x06

// Sabitler
#define MAX_COLLECT_DATA_ENTRIES 10
#define GUARD_TIME_MS            60000
#define PACKET_TIMEOUT_MS        10000
#define MAX_METER_SERIAL_LEN     12

// UDP socket (udp_meter_task içinde oluşturulur, extern olarak kullanılır)
extern int meter_sock;

// Veri yapıları
typedef struct {
    char nodeSerial[16];
    char meterSerial[16];
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
    bool finalAckSent;              // final ACK gönderildi mi?
    TickType_t startTime;
    TickType_t guardTime;
    TickType_t packetTimeout;
    TickType_t lastPacketTime;      // son paket alınma zamanı
} collect_data_t;

// Fonksiyon prototipleri (sadece dışarıdan çağrılanlar)
void udp_meter_task(void *pvParameters);
void handle_configure(const uint8_t *data, int len, struct sockaddr_in6 *src_addr);
void handle_data(const uint8_t *data, int len, struct sockaddr_in6 *src_addr);
void handle_error(const uint8_t *data, int len, struct sockaddr_in6 *src_addr);
void check_all_collect_data(void);
void process_completed_data(collect_data_t *cd);

// Yardımcı fonksiyonlar (diğer dosyalarda kullanılabilir)
uint8_t convert_baud_to_byte(int baud);
int convert_byte_to_baud(uint8_t indicator);
void generate_ulid(char *out);

#endif