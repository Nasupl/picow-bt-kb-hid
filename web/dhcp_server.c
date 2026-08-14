/*
 * Minimal DHCP server derived from the MicroPython DHCP server.
 * Copyright (c) 2018-2019 Damien P. George. MIT licensed.
 */
#include "dhcp_server.h"

#include <stddef.h>
#include <string.h>

#include "lwip/def.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "pico/time.h"
#include "usb_serial.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY 2
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_OPTION_SUBNET 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_DNS 6
#define DHCP_OPTION_REQUESTED_IP 50
#define DHCP_OPTION_LEASE_TIME 51
#define DHCP_OPTION_MESSAGE_TYPE 53
#define DHCP_OPTION_SERVER_ID 54
#define DHCP_OPTION_END 255
#define DHCP_LEASE_BASE 16
#define DHCP_MAGIC_COOKIE 0x63825363u
#define DHCP_FIXED_SIZE 240
#define DHCP_MIN_RESPONSE_SIZE 300
#define DHCP_OFFER_HOLD_MS 60000u
#define DHCP_LEASE_TIME_SECONDS 86400u

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t siaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t cookie;
    uint8_t options[312];
} dhcp_message_t;

static const uint8_t *find_option(const uint8_t *options, size_t length,
                                  uint8_t wanted) {
    size_t offset = 0;
    while (offset < length) {
        uint8_t option = options[offset++];
        if (option == DHCP_OPTION_END) break;
        if (option == 0) continue;
        if (offset >= length) break;
        uint8_t option_length = options[offset++];
        if (offset + option_length > length) break;
        if (option == wanted) return &options[offset - 2];
        offset += option_length;
    }
    return NULL;
}

static void write_option(uint8_t **cursor, uint8_t option,
                         const void *value, uint8_t length) {
    *(*cursor)++ = option;
    *(*cursor)++ = length;
    memcpy(*cursor, value, length);
    *cursor += length;
}

static int find_lease(dhcp_server_t *server, const uint8_t mac[6]) {
    int free_lease = -1;
    int oldest_lease = 0;
    uint32_t now = (uint32_t) to_ms_since_boot(get_absolute_time());
    for (int i = 0; i < DHCP_SERVER_MAX_LEASES; ++i) {
        if (memcmp(server->leases[i].mac, mac, 6) == 0) return i;
        static const uint8_t empty[6] = {0};
        bool unused = memcmp(server->leases[i].mac, empty, 6) == 0;
        bool expired = !unused &&
            (int32_t) (server->leases[i].expiry_ms - now) <= 0;
        if (free_lease < 0 && (unused || expired)) {
            free_lease = i;
        }
        if ((int32_t) (server->leases[i].expiry_ms -
                       server->leases[oldest_lease].expiry_ms) < 0) {
            oldest_lease = i;
        }
    }
    // If all leases are active, recycle the one that expires first. This
    // prevents randomized client MAC addresses from exhausting the AP until
    // the next reboot.
    return free_lease >= 0 ? free_lease : oldest_lease;
}

static void receive_dhcp(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *address, u16_t port) {
    (void) pcb;
    (void) address;
    (void) port;
    dhcp_server_t *server = arg;
    dhcp_message_t message = {0};
    size_t received = pbuf_copy_partial(p, &message, sizeof(message), 0);
    if (received < DHCP_FIXED_SIZE + 3) {
        pbuf_free(p);
        return;
    }
    pbuf_free(p);
    if (message.op != DHCP_BOOTREQUEST ||
        message.cookie != lwip_htonl(DHCP_MAGIC_COOKIE)) return;

    size_t options_length = received - offsetof(dhcp_message_t, options);
    const uint8_t *message_type = find_option(
        message.options, options_length, DHCP_OPTION_MESSAGE_TYPE);
    if (message_type == NULL || message_type[1] != 1) return;

    int lease = find_lease(server, message.chaddr);
    if (lease < 0) return;
    uint8_t reply_type;
    if (message_type[2] == DHCP_DISCOVER) {
        reply_type = DHCP_OFFER;
        // Reserve the offered slot so a following request from this client
        // resolves to the same address even if another discovery arrives.
        memcpy(server->leases[lease].mac, message.chaddr, 6);
        server->leases[lease].expiry_ms =
            (uint32_t) to_ms_since_boot(get_absolute_time()) +
            DHCP_OFFER_HOLD_MS;
        usb_serial_printf(
            "[WEB] DHCP DISCOVER mac=%02x:%02x:%02x:%02x:%02x:%02x offer=192.168.4.%u\r\n",
            message.chaddr[0], message.chaddr[1], message.chaddr[2],
            message.chaddr[3], message.chaddr[4], message.chaddr[5],
            (unsigned int) (DHCP_LEASE_BASE + lease));
    } else if (message_type[2] == DHCP_REQUEST) {
        const uint8_t *requested = find_option(
            message.options, options_length, DHCP_OPTION_REQUESTED_IP);
        if (requested != NULL && requested[1] == 4 &&
            requested[5] != DHCP_LEASE_BASE + lease) return;
        memcpy(server->leases[lease].mac, message.chaddr, 6);
        server->leases[lease].expiry_ms =
            (uint32_t) to_ms_since_boot(get_absolute_time()) +
            DHCP_LEASE_TIME_SECONDS * 1000u;
        reply_type = DHCP_ACK;
        usb_serial_printf(
            "[WEB] DHCP REQUEST mac=%02x:%02x:%02x:%02x:%02x:%02x ack=192.168.4.%u\r\n",
            message.chaddr[0], message.chaddr[1], message.chaddr[2],
            message.chaddr[3], message.chaddr[4], message.chaddr[5],
            (unsigned int) (DHCP_LEASE_BASE + lease));
    } else {
        return;
    }

    const ip4_addr_t *server_ip = netif_ip4_addr(server->netif);
    const ip4_addr_t *netmask = netif_ip4_netmask(server->netif);
    memcpy(message.yiaddr, &server_ip->addr, 4);
    message.yiaddr[3] = (uint8_t) (DHCP_LEASE_BASE + lease);
    memcpy(message.siaddr, &server_ip->addr, 4);
    message.op = DHCP_BOOTREPLY;
    message.cookie = lwip_htonl(DHCP_MAGIC_COOKIE);

    uint8_t *cursor = message.options;
    write_option(&cursor, DHCP_OPTION_MESSAGE_TYPE, &reply_type, 1);
    write_option(&cursor, DHCP_OPTION_SERVER_ID, &server_ip->addr, 4);
    write_option(&cursor, DHCP_OPTION_SUBNET, &netmask->addr, 4);
    write_option(&cursor, DHCP_OPTION_ROUTER, &server_ip->addr, 4);
    write_option(&cursor, DHCP_OPTION_DNS, &server_ip->addr, 4);
    uint32_t lease_time = lwip_htonl(DHCP_LEASE_TIME_SECONDS);
    write_option(&cursor, DHCP_OPTION_LEASE_TIME, &lease_time, 4);
    *cursor++ = DHCP_OPTION_END;

    size_t response_size = offsetof(dhcp_message_t, options) +
                           (size_t) (cursor - message.options);
    if (response_size < DHCP_MIN_RESPONSE_SIZE) {
        response_size = DHCP_MIN_RESPONSE_SIZE;
    }
    struct pbuf *response = pbuf_alloc(PBUF_TRANSPORT, response_size, PBUF_RAM);
    if (response == NULL) return;
    memcpy(response->payload, &message, response_size);
    ip_addr_t broadcast;
    IP_ADDR4(&broadcast, 255, 255, 255, 255);
    udp_sendto_if(server->pcb, response, &broadcast, DHCP_CLIENT_PORT,
                  server->netif);
    pbuf_free(response);
}

bool dhcp_server_init(dhcp_server_t *server, struct netif *netif) {
    if (server == NULL || netif == NULL) return false;
    memset(server, 0, sizeof(*server));
    server->netif = netif;
    server->pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (server->pcb == NULL) return false;
    if (udp_bind(server->pcb, IP_ANY_TYPE, DHCP_SERVER_PORT) != ERR_OK) {
        udp_remove(server->pcb);
        server->pcb = NULL;
        return false;
    }
    udp_bind_netif(server->pcb, netif);
    udp_recv(server->pcb, receive_dhcp, server);
    return true;
}

void dhcp_server_deinit(dhcp_server_t *server) {
    if (server != NULL && server->pcb != NULL) {
        udp_remove(server->pcb);
        server->pcb = NULL;
    }
}
