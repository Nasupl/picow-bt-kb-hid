#include "web_config.h"

#include <stdio.h>
#include <string.h>

#include "bluetooth_control.h"
#include "control_json.h"
#include "dhcp_server.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "usb_serial.h"
#include "web_ui.h"

#define WEB_AP_SSID "PicoW-Keyboard-Setup"
#define WEB_AP_PASSWORD "pico-keyboard"
#define WEB_MAX_CLIENTS 2
#define WEB_REQUEST_SIZE 1536
#define WEB_RESPONSE_HEADER_SIZE 256

typedef struct {
    bool used;
    struct tcp_pcb *pcb;
    size_t request_length;
    uint8_t age_polls;
    char request[WEB_REQUEST_SIZE];
    char response_header[WEB_RESPONSE_HEADER_SIZE];
} web_client_t;

static web_client_t clients[WEB_MAX_CLIENTS];
static struct tcp_pcb *listener;
static dhcp_server_t dhcp_server;
// The RP2040 default main stack is only 2 KiB. Keep the status working set in
// BSS rather than placing more than 4 KiB in handle_request's stack frame.
static bluetooth_control_snapshot_t status_snapshot;
static char status_json[CONTROL_JSON_MAX_SNAPSHOT_SIZE];
static bool cdc_was_connected;

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool parse_address(const char *text, uint8_t address[6]) {
    for (size_t i = 0; i < 6; ++i) {
        int high = hex_value(text[0]);
        int low = hex_value(text[1]);
        if (high < 0 || low < 0) return false;
        address[i] = (uint8_t) ((high << 4) | low);
        text += 2;
        if (i != 5 && *text++ != ':') return false;
    }
    return *text == ' ' || *text == '\0' || *text == '&';
}

static err_t close_client(web_client_t *client) {
    struct tcp_pcb *pcb = client->pcb;
    memset(client, 0, sizeof(*client));
    if (pcb == NULL) return ERR_OK;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t poll_client(void *arg, struct tcp_pcb *pcb) {
    (void) pcb;
    web_client_t *client = arg;
    if (client == NULL) return ERR_OK;
    if (++client->age_polls >= 5) {
        return close_client(client);
    }
    return ERR_OK;
}

static void client_error(void *arg, err_t error) {
    (void) error;
    web_client_t *client = arg;
    if (client != NULL) memset(client, 0, sizeof(*client));
}

static err_t send_response(web_client_t *client, int status,
                           const char *content_type, const char *body,
                           size_t body_length) {
    const char *reason = status == 200 ? "OK" :
                         status == 202 ? "Accepted" :
                         status == 400 ? "Bad Request" :
                         status == 404 ? "Not Found" :
                         status == 405 ? "Method Not Allowed" :
                         status == 503 ? "Service Unavailable" : "Error";
    int header_length = snprintf(
        client->response_header, sizeof(client->response_header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, reason, content_type, (unsigned int) body_length);
    if (header_length < 0 || (size_t) header_length >=
                                 sizeof(client->response_header)) {
        return close_client(client);
    }
    if (tcp_write(client->pcb, client->response_header, (size_t) header_length,
                  TCP_WRITE_FLAG_COPY) != ERR_OK ||
        tcp_write(client->pcb, body, body_length, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return close_client(client);
    }
    tcp_output(client->pcb);
    return close_client(client);
}

static err_t respond_json(web_client_t *client, int status, const char *json) {
    return send_response(client, status, "application/json; charset=utf-8",
                         json, strlen(json));
}

static err_t handle_request(web_client_t *client) {
    char method[8] = {0};
    char target[128] = {0};
    if (sscanf(client->request, "%7s %127s", method, target) != 2) {
        return respond_json(client, 400, "{\"error\":\"invalid request\"}");
    }
    if (strcmp(method, "GET") == 0 && strcmp(target, "/api/status") == 0) {
        bluetooth_control_get_snapshot(&status_snapshot);
        size_t length = 0;
        if (!control_json_write_snapshot(&status_snapshot, status_json,
                                         sizeof(status_json), &length)) {
            return respond_json(client, 503,
                                "{\"error\":\"status too large\"}");
        }
        return send_response(client, 200, "application/json; charset=utf-8",
                             status_json, length);
    }
    if (strcmp(method, "GET") == 0 &&
        (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0)) {
        return send_response(client, 200, "text/html; charset=utf-8",
                             web_ui_html, web_ui_html_length);
    }
    if (strcmp(method, "POST") != 0) {
        return respond_json(client, 405,
                            "{\"error\":\"method not allowed\"}");
    }

    bluetooth_control_request_t request = {0};
    if (strcmp(target, "/api/scan") == 0) {
        request.action = BLUETOOTH_CONTROL_SCAN;
    } else if (strncmp(target, "/api/connect?address=", 21) == 0 &&
               parse_address(target + 21, request.address)) {
        request.action = BLUETOOTH_CONTROL_CONNECT;
    } else if (strcmp(target, "/api/disconnect") == 0) {
        request.action = BLUETOOTH_CONTROL_DISCONNECT;
    } else if (strcmp(target, "/api/reconnect") == 0) {
        request.action = BLUETOOTH_CONTROL_RECONNECT;
    } else if (strcmp(target, "/api/forget") == 0) {
        request.action = BLUETOOTH_CONTROL_FORGET;
    } else {
        return respond_json(client, 404,
                            "{\"error\":\"unknown endpoint\"}");
    }
    if (!bluetooth_control_submit(&request)) {
        return respond_json(client, 503,
                            "{\"error\":\"control queue full\"}");
    }
    return respond_json(client, 202, "{\"accepted\":true}");
}

static err_t receive_request(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                             err_t error) {
    (void) pcb;
    web_client_t *client = arg;
    if (p == NULL || error != ERR_OK) {
        if (p != NULL) pbuf_free(p);
        return close_client(client);
    }
    tcp_recved(client->pcb, p->tot_len);
    size_t available = sizeof(client->request) - 1 - client->request_length;
    size_t copied = pbuf_copy_partial(p, client->request + client->request_length,
                                      available, 0);
    client->request_length += copied;
    client->request[client->request_length] = '\0';
    pbuf_free(p);
    if (strstr(client->request, "\r\n\r\n") != NULL) {
        return handle_request(client);
    } else if (available == copied) {
        return respond_json(client, 400,
                            "{\"error\":\"request too large\"}");
    }
    return ERR_OK;
}

static err_t accept_client(void *arg, struct tcp_pcb *pcb, err_t error) {
    (void) arg;
    if (error != ERR_OK) return error;
    for (size_t i = 0; i < WEB_MAX_CLIENTS; ++i) {
        if (!clients[i].used) {
            clients[i].used = true;
            clients[i].pcb = pcb;
            tcp_arg(pcb, &clients[i]);
            tcp_recv(pcb, receive_request);
            tcp_err(pcb, client_error);
            // tcp_poll runs every ~1 second with interval 2. Reclaim clients
            // that do not complete an HTTP header within about 5 seconds.
            tcp_poll(pcb, poll_client, 2);
            return ERR_OK;
        }
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

bool web_config_init(void) {
    cyw43_arch_enable_ap_mode(WEB_AP_SSID, WEB_AP_PASSWORD,
                              CYW43_AUTH_WPA2_AES_PSK);
    ip_addr_t gateway;
    ip_addr_t netmask;
    IP_ADDR4(&gateway, 192, 168, 4, 1);
    IP_ADDR4(&netmask, 255, 255, 255, 0);
    dhcp_server_init(&dhcp_server, &cyw43_state.netif[CYW43_ITF_AP],
                     &gateway, &netmask);
    listener = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (listener == NULL || tcp_bind(listener, IP_ANY_TYPE, 80) != ERR_OK) {
        usb_serial_printf("[WEB] HTTP listener setup failed\r\n");
        return false;
    }
    listener = tcp_listen_with_backlog(listener, WEB_MAX_CLIENTS);
    if (listener == NULL) {
        usb_serial_printf("[WEB] HTTP listen failed\r\n");
        return false;
    }
    tcp_accept(listener, accept_client);
    usb_serial_printf("[WEB] AP SSID=%s password=%s URL=http://192.168.4.1/\r\n",
                      WEB_AP_SSID, WEB_AP_PASSWORD);
    return true;
}

void web_config_task(void) {
    bool connected = usb_serial_connected();
    if (connected && !cdc_was_connected) {
        usb_serial_printf(
            "[WEB] AP SSID=%s URL=http://192.168.4.1/ "
            "DHCP discovers=%lu requests=%lu acks=%lu\r\n",
            WEB_AP_SSID, (unsigned long) dhcp_server.discover_count,
            (unsigned long) dhcp_server.request_count,
            (unsigned long) dhcp_server.ack_count);
    }
    cdc_was_connected = connected;
}
