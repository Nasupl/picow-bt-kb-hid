#ifndef WEB_DHCP_SERVER_H
#define WEB_DHCP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "lwip/netif.h"
#include "lwip/udp.h"

#define DHCP_SERVER_MAX_LEASES 8

typedef struct {
    uint8_t mac[6];
} dhcp_lease_t;

typedef struct {
    struct udp_pcb *pcb;
    struct netif *netif;
    dhcp_lease_t leases[DHCP_SERVER_MAX_LEASES];
} dhcp_server_t;

bool dhcp_server_init(dhcp_server_t *server, struct netif *netif);
void dhcp_server_deinit(dhcp_server_t *server);

#endif
