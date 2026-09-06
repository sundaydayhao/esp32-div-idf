#include "captive_dns.h"

#include <fcntl.h>

#include "dns_packet.h"
#include "lwip/sockets.h"
void captive_dns_stop(captive_dns_t* server) {
  if (server && server->socket >= 0) {
    close(server->socket);
    server->socket = -1;
  }
}
esp_err_t captive_dns_start(captive_dns_t* server, uint32_t address) {
  if (!server) return ESP_ERR_INVALID_ARG;
  captive_dns_stop(server);
  server->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (server->socket < 0) return ESP_FAIL;
  struct sockaddr_in bind_address = {
      .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = address};
  if (bind(server->socket, (struct sockaddr*)&bind_address,
           sizeof(bind_address)) < 0 ||
      fcntl(server->socket, F_SETFL, O_NONBLOCK) < 0) {
    captive_dns_stop(server);
    return ESP_FAIL;
  }
  server->address = address;
  return ESP_OK;
}
void captive_dns_poll(captive_dns_t* server) {
  if (!server || server->socket < 0) return;
  for (int budget = 0; budget < 4; ++budget) {
    uint8_t bytes[512];
    struct sockaddr_in peer = {0};
    socklen_t length = sizeof(peer);
    int count = recvfrom(server->socket, bytes, sizeof(bytes) - 16, 0,
                         (struct sockaddr*)&peer, &length);
    if (count < 12) return;
    size_t size = dns_reply(bytes, count, sizeof(bytes), server->address);
    if (size)
      sendto(server->socket, bytes, size, 0, (struct sockaddr*)&peer, length);
  }
}
