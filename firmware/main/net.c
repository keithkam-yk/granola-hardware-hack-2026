// Finding the game and staying on it.
//
// The board is a controller, not a server: it joins the same wifi as the host,
// shouts for it, and holds a socket open. Nothing about where the host is has to
// be configured, because a broadcast on the local network answers that question
// better than a stored address that goes stale the next time DHCP moves things
// around.
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "link.h"
#include "net.h"

#define NVS_NAMESPACE   "dogfight"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define NVS_KEY_HOST    "host"
#define NVS_KEY_PORT    "hport"

// The host answers a broadcast on this port with the TCP port to come back on.
#define DISCOVERY_PORT  41234
#define DISCOVERY_ASK   "DOGFIGHT?"
#define DISCOVERY_REPLY "DOGFIGHT "

#define GOT_IP_BIT      BIT0

static EventGroupHandle_t s_events;

// A host that has been named explicitly. Broadcast discovery only reaches the
// local link, and an access point that hands out more than one subnet — guest
// networks routinely do — leaves the board and the laptop unable to shout at
// each other even though they can route perfectly well.
static char s_host[16];
static uint16_t s_host_port;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        link_sendf("#net wifi disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        link_sendf("#net ip=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

// Returns the host's address, or false if nobody answered this round.
static bool discover_host(struct sockaddr_in *host, uint16_t *tcp_port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;

    int broadcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(fd, DISCOVERY_ASK, strlen(DISCOVERY_ASK), 0, (struct sockaddr *)&to, sizeof(to));

    char reply[64];
    socklen_t from_len = sizeof(*host);
    int n = recvfrom(fd, reply, sizeof(reply) - 1, 0, (struct sockaddr *)host, &from_len);
    close(fd);

    if (n <= (int)strlen(DISCOVERY_REPLY)) return false;
    reply[n] = '\0';
    if (strncmp(reply, DISCOVERY_REPLY, strlen(DISCOVERY_REPLY)) != 0) return false;

    *tcp_port = (uint16_t)atoi(reply + strlen(DISCOVERY_REPLY));
    return *tcp_port != 0;
}

// Reads until the host goes away. Lines arrive split across packets, so the tail
// of a partial line has to survive into the next read.
static void pump(int fd)
{
    char pending[256];
    size_t used = 0;

    while (1) {
        char chunk[256];
        int n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return;

        for (int i = 0; i < n; i++) {
            if (chunk[i] == '\n' || used == sizeof(pending) - 1) {
                pending[used] = '\0';
                if (used) link_deliver(pending);
                used = 0;
            } else if (chunk[i] != '\r') {
                pending[used++] = chunk[i];
            }
        }
    }
}

// Prefers an address that was given to us, and shouts for one otherwise.
static bool find_host(struct sockaddr_in *host, uint16_t *port)
{
    if (s_host[0] != '\0') {
        memset(host, 0, sizeof(*host));
        host->sin_family = AF_INET;
        host->sin_addr.s_addr = inet_addr(s_host);
        *port = s_host_port;
        if (host->sin_addr.s_addr != INADDR_NONE) return true;
    }
    return discover_host(host, port);
}

static void host_task(void *arg)
{
    int quiet_rounds = 0;

    while (1) {
        xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        struct sockaddr_in host;
        uint16_t port = 0;
        if (!find_host(&host, &port)) {
            // Silence here reads as a dead board, so say something occasionally
            // without turning the link into a log.
            if (++quiet_rounds % 10 == 0) {
                link_sendf("#net no host yet; send: !host ip=A.B.C.D");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        quiet_rounds = 0;
        host.sin_port = htons(port);

        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (connect(fd, (struct sockaddr *)&host, sizeof(host)) != 0) {
            close(fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Samples are small and frequent, which is exactly the traffic Nagle
        // ruins: without this the stick lags by tens of milliseconds.
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval send_timeout = {.tv_sec = 2};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

        link_sendf("#net host " IPSTR ":%u", IP2STR((esp_ip4_addr_t *)&host.sin_addr), port);
        link_attach_socket(fd);
        pump(fd);
        link_detach_socket();
        close(fd);
        link_sendf("#net host gone, looking again");
    }
}

void net_set_host(const char *ip, uint16_t port)
{
    snprintf(s_host, sizeof(s_host), "%s", ip);
    s_host_port = port;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_HOST, s_host);
        nvs_set_u16(nvs, NVS_KEY_PORT, port);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    link_sendf("#net host set to %s:%u", s_host, port);
}

void net_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_PASS, password);
    nvs_commit(nvs);
    nvs_close(nvs);

    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password);
    esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_wifi_disconnect();
    esp_wifi_connect();
    link_sendf("#net joining %s", ssid);
}

// True if a network was stored; the caller only needs to know whether to tell
// the pilot that the board has nowhere to go yet.
static bool load_credentials(wifi_config_t *config)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t host_len = sizeof(s_host);
    if (nvs_get_str(nvs, NVS_KEY_HOST, s_host, &host_len) != ESP_OK) s_host[0] = '\0';
    if (nvs_get_u16(nvs, NVS_KEY_PORT, &s_host_port) != ESP_OK) s_host_port = 41235;

    size_t ssid_len = sizeof(config->sta.ssid);
    size_t pass_len = sizeof(config->sta.password);
    bool ok = nvs_get_str(nvs, NVS_KEY_SSID, (char *)config->sta.ssid, &ssid_len) == ESP_OK
              && nvs_get_str(nvs, NVS_KEY_PASS, (char *)config->sta.password, &pass_len) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

esp_err_t net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t config = {0};
    bool have = load_credentials(&config);
    if (have) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));

    ESP_ERROR_CHECK(esp_wifi_start());

    // Modem power save parks the radio between beacons: ICMP to this board sat
    // at a 6 ms floor with a 105 ms average, and the stick arrived in bursts
    // instead of a stream. It has to be set after the driver is started to
    // stick. A controller runs for one session, so the power is not worth it.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (!have) link_sendf("#net no network stored; send: !wifi ssid=NAME pass=SECRET");

    return xTaskCreate(host_task, "net_host", 4096, NULL, 4, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
