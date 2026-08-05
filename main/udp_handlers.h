#ifndef UDP_HANDLERS_H
#define UDP_HANDLERS_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef void (*udp_handler_cb_t)(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr);

// Handler bildirimleri
void handle_module_support(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr);
void handle_timestamp(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr);
void handle_route(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr);
void handle_meter_read(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr);

// Başlatma fonksiyonu
void init_udp_handlers(void);
int get_udp_socket_for_port(uint16_t port);
#endif