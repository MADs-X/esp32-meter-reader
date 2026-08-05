#ifndef OBIS_MANAGER_H
#define OBIS_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "esp_err.h"
#include "esp_log.h"

// ============================================================
// SISTEM VE BELLEK SINIRLARI
// ============================================================
#define OBIS_MAX_NODES             10
#define OBIS_MAX_CODES_PER_READER  10
#define OBIS_CODE_MAX_LEN          16
#define OBIS_VALUE_MAX_LEN         128
#define SERIAL_MAX_LEN             16
#define OBIS_MAX_REQUEST_LEN       62
#define OBIS_MAX_FRAMES            8

// ============================================================
// DURUM MAKİNESİ (FSM) TANIMLARI
// ============================================================
typedef enum {
    OBIS_STATE_REQUESTING = 0,
    OBIS_STATE_PROCESSING = 1,
    OBIS_STATE_WAITING    = 2,
    OBIS_STATE_INET_FAIL  = 3,
    OBIS_STATE_SUCCEED    = 4,
    OBIS_STATE_FAILED     = 5,
    OBIS_STATE_READ_ERROR = 6
} obis_state_t;

// ============================================================
// VERİ YAPILARI
// ============================================================
typedef struct {
    uint8_t temp_index;  
    char code[OBIS_CODE_MAX_LEN];
    char value_buf[OBIS_VALUE_MAX_LEN];
    bool is_received;

    uint8_t frame_count;
    uint8_t received_frame_mask;
    char frame_buffers[OBIS_MAX_FRAMES][OBIS_VALUE_MAX_LEN];
    uint16_t frame_lens[OBIS_MAX_FRAMES];
} obis_code_t;

typedef struct {
    char meter_serial[SERIAL_MAX_LEN];
    uint8_t start_baud_rate;
    obis_code_t codes[OBIS_MAX_CODES_PER_READER];
    uint8_t active_code_count;
} obis_info_t;

typedef struct {
    bool is_active;
    char node_serial[SERIAL_MAX_LEN];
    struct sockaddr_in6 node_addr;
    
    int read_period;
    int remaining_time_sec;
    uint16_t seq_number;
    uint16_t last_sent_seq;    // Son gönderilen isteğin seq değeri (yanıt eşleşmesi için)
    obis_state_t state;
    uint8_t total_request_attempts;
    uint8_t send_req_counter;
    uint8_t send_req_limit;
    uint8_t wait_counter;
    
    obis_info_t info;
} obis_reader_t;



#define MR_LOAD_REQUEST 0x0C
#define MR_LOAD_CONF    0x0D
#define MR_LOAD_DATA    0x0E
#define MR_READ_ERROR   0x06
#define MR_MISS 0x05

typedef struct {
    bool last_succeed;            // B: 1. bit
    uint8_t seq_number;           // C: Sequence Number
    char meter_serial[32];        // D: Meter Serial (Max 31 karakter varsayıldı)
    uint8_t date_string_len;      // E: Date String Uzunluğu
    char *date_string;            // F: Date String (Tarih Parametresi)
    uint8_t baud_rate_indicator;  // G: Baud Rate Göstergesi
} load_profile_request_t;
//-----------------------------------------

#define PERIODIC_OBIS_CONF                  0x14 
#define PERIODIC_OBIS_CONF_ACK              0x15 
#define PERIODIC_OBIS_PACKET                0x16 
#define PERIODIC_OBIS_PACKET_ACK            0x17 
#define DELETE_PERIODIC_OBIS_CODES          0x18 
#define DELETE_PERIODIC_OBIS_CODES_ACK      0x19 
#define CLEAR_PERIODIC_OBIS_CODE_MEM        0x22 
#define CLEAR_PERIODIC_OBIS_CODE_MEM_ACK    0x23 

// Periyodik Okuma 
typedef struct {
    bool is_active;
    char meter_serial[32];
    uint8_t baud_rate_indicator;
    uint8_t interval;          // Okuma periyodu (dakika veya saniye)
    uint8_t total_obis_count;
    uint16_t total_length;
    
    uint8_t seq_num;                
    uint8_t expected_packet_count;

    char **obis_codes;         
    uint8_t stored_obis_count; // Şu ana kadar cihaza ulaşan paket sayısı
    
    // FreeRTOS Timer için son okuma zamanı
    uint32_t last_read_timestamp; 
} periodic_task_t;

// Global veya statik bir dizi/liste ile periyodik görevleri tutacağız
#define MAX_PERIODIC_METERS 10
extern periodic_task_t periodic_tasks[MAX_PERIODIC_METERS];



//-----------------------------------MAD



void obis_manager_init(int udp_sock);
void obis_manager_task(void *pvParameters);
void obis_process_incoming_packet(const char* node_serial, const char* meter_serial, 
                                  uint16_t seq, uint8_t frame_count, uint8_t frame_id, 
                                  uint8_t obis_count, const uint8_t* data, uint16_t data_len);

esp_err_t obis_manager_add_test_node(const char *node_serial,
                                     const struct sockaddr_in6 *node_addr,
                                     int read_period,
                                     const char *meter_serial,
                                     uint8_t start_baud_rate,
                                     const char *obis_codes[], int obis_count);
void obis_set_state_by_addr(const struct sockaddr_in6 *src_addr, obis_state_t new_state);
void obis_notify_node_online(const struct sockaddr_in6 *src_addr);
void process_load_read_request(const uint8_t *data, uint16_t len, struct sockaddr_in6 *source_addr);
//periodikler mad
bool obis_mgr_add_periodic_task(const char *meter_serial, uint8_t baud, uint8_t interval, uint8_t total_obis, uint16_t total_len, uint8_t seq, uint8_t pkt_count);
bool obis_mgr_add_periodic_obis(const char *meter_serial, const char *obis_code);
void obis_mgr_delete_periodic_task(const char *meter_serial);
#endif // OBIS_MANAGER_H