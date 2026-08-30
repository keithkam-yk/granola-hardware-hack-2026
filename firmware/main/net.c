// Finding the game, and getting back to it without being asked anything.
//
// A controller should be something you switch on. So it remembers every network
// it has ever been told about and joins whichever one is actually in the room,
// remembers where the game was and looks there first, and shouts for it two
// ways when that fails. Nothing here needs a laptop at play time; the USB
// commands exist only to teach it a network it has never seen.
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "hud.h"
#include "link.h"
#include "net.h"

#define NVS_NAMESPACE   "dogfight"
#define NVS_KEY_NETS    "nets"
#define NVS_KEY_HOST    "host"
#define NVS_KEY_PORT    "hport"

#define DISCOVERY_PORT  41234
#define DISCOVERY_ASK   "DOGFIGHT?"
#define DISCOVERY_REPLY "DOGFIGHT "

#define GOT_IP_BIT      BIT0
#define CONNECT_TIMEOUT_MS 1500

// Enough for home, the venue, and a phone hotspot, which is every situation a
// controller has ever actually been in.
#define MAX_NETWORKS    4

// Enough of a scan to find a known network in a crowded room.
#define MAX_SCAN_RESULTS 20

typedef struct {
    char ssid[33];
    char password[65];
} network_t;

static EventGroupHandle_t s_events;

// Joining runs on its own task. Scanning is blocking and needs kilobytes of
// stack for the results, and doing it inside the wifi event handler overflowed
// the system event task and put the board in a boot loop.
static SemaphoreHandle_t s_rejoin;
static network_t s_networks[MAX_NETWORKS];
static int s_network_count;

// Where the game was last time. Tried before shouting, because a laptop keeps
// its address for days at a time and asking is slower than remembering.
static char s_host[16];
static uint16_t s_host_port = 41235;

static void save_networks(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, NVS_KEY_NETS, s_networks, sizeof(network_t) * s_network_count);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void remember_host(const char *ip, uint16_t port)
{
    if (strcmp(s_host, ip) == 0 && s_host_port == port) return;

    snprintf(s_host, sizeof(s_host), "%s", ip);
    s_host_port = port;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_HOST, s_host);
        nvs_set_u16(nvs, NVS_KEY_PORT, port);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool s_migrated;

static void load_stored(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;

    size_t size = sizeof(s_networks);
    if (nvs_get_blob(nvs, NVS_KEY_NETS, s_networks, &size) == ESP_OK) {
        s_network_count = size / sizeof(network_t);
    } else {
        // Boards flashed before this held one network under its own pair of
        // keys. Changing the layout without carrying those across would quietly
        // forget the wifi of every board already out there, and the board
        // cannot be told a new one without being brought back to a cable.
        size_t ssid_len = sizeof(s_networks[0].ssid);
        size_t pass_len = sizeof(s_networks[0].password);
        if (nvs_get_str(nvs, "ssid", s_networks[0].ssid, &ssid_len) == ESP_OK
                && nvs_get_str(nvs, "pass", s_networks[0].password, &pass_len) == ESP_OK) {
            s_network_count = 1;
            s_migrated = true;
        }
    }

    size_t host_len = sizeof(s_host);
    if (nvs_get_str(nvs, NVS_KEY_HOST, s_host, &host_len) != ESP_OK) s_host[0] = '\0';
    nvs_get_u16(nvs, NVS_KEY_PORT, &s_host_port);
    nvs_close(nvs);
}

// Joins whichever known network is actually in the room, strongest first, so
// carrying the board somewhere else needs nothing said to it.
static bool join_best_network(void)
{
    if (s_network_count == 0) return false;

    hud_set_link("scanning");
    wifi_scan_config_t scan = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        link_sendf("#net scan failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        link_sendf("#net scan saw nothing");
        return false;
    }
    if (found > MAX_SCAN_RESULTS) found = MAX_SCAN_RESULTS;

    // A scan record is a few hundred bytes and there are twenty of them, which
    // is more than belongs on any task's stack.
    wifi_ap_record_t *records = calloc(found, sizeof(wifi_ap_record_t));
    if (!records) return false;
    if (esp_wifi_scan_get_ap_records(&found, records) != ESP_OK) {
        free(records);
        return false;
    }

    // Records come back strongest first, so the first known one is the best one.
    bool joined = false;
    for (int i = 0; i < found && !joined; i++) {
        for (int n = 0; n < s_network_count; n++) {
            if (strcmp((char *)records[i].ssid, s_networks[n].ssid) != 0) continue;

            // The driver's fields are one byte shorter than ours, which is
            // exactly the room a terminator needs, so this copies rather than
            // formats.
            wifi_config_t config = {0};
            memcpy(config.sta.ssid, s_networks[n].ssid, sizeof(config.sta.ssid));
            memcpy(config.sta.password, s_networks[n].password, sizeof(config.sta.password));
            esp_wifi_set_config(WIFI_IF_STA, &config);

            link_sendf("#net joining %s at %d dBm", s_networks[n].ssid, records[i].rssi);
            hud_set_link(s_networks[n].ssid);
            esp_wifi_connect();
            joined = true;
            break;
        }
    }
    free(records);

    if (!joined) {
        link_sendf("#net none of %d known networks are in range", s_network_count);
        hud_set_link("no known wifi");
    }
    return joined;
}

// Scanning is how the board chooses between several known networks. It is not
// how it connects, and treating it as a requirement made a failed scan into a
// board that never joins anything. When it cannot choose, it just tries them.
static bool join_blind(void)
{
    static int next;

    if (s_network_count == 0) return false;
    int slot = next++ % s_network_count;

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, s_networks[slot].ssid, sizeof(config.sta.ssid));
    memcpy(config.sta.password, s_networks[slot].password, sizeof(config.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &config);

    link_sendf("#net trying %s without a scan", s_networks[slot].ssid);
    hud_set_link(s_networks[slot].ssid);
    return esp_wifi_connect() == ESP_OK;
}

// Everything that has to scan or block happens here, never in an event handler.
static void join_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_rejoin, portMAX_DELAY);
        if (join_best_network()) continue;
        if (join_blind()) continue;

        vTaskDelay(pdMS_TO_TICKS(5000));
        xSemaphoreGive(s_rejoin);   // nothing yet; look again shortly
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // Handlers only ever signal. They run on the system event task, whose stack
    // is small and whose progress everything else depends on.
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xSemaphoreGive(s_rejoin);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        hud_set_link("wifi lost");
        // Rescan rather than retry blindly: it may have dropped because the
        // board has been carried somewhere with a different network in it.
        xSemaphoreGive(s_rejoin);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        link_sendf("#net ip=" IPSTR, IP2STR(&event->ip_info.ip));
        hud_set_link("looking for game");
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

// Asks on the local link and, if a host has been seen before, on that host's
// link as well. An access point handing out two subnets blocks the first and
// usually passes the second, which is the whole reason a host had to be named
// by hand before.
static bool ask_for_host(struct sockaddr_in *host, uint16_t *tcp_port)
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

    if (s_host[0] != '\0') {
        // The remembered host's own broadcast address: everything up to the
        // last dot, then 255. Finds the game again after DHCP moved it.
        char directed[16];
        snprintf(directed, sizeof(directed), "%s", s_host);
        char *last_dot = strrchr(directed, '.');
        if (last_dot) {
            snprintf(last_dot, sizeof(directed) - (last_dot - directed), ".255");
            to.sin_addr.s_addr = inet_addr(directed);
            if (to.sin_addr.s_addr != INADDR_NONE) {
                sendto(fd, DISCOVERY_ASK, strlen(DISCOVERY_ASK), 0,
                       (struct sockaddr *)&to, sizeof(to));
            }
        }
    }

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

// lwip's blocking connect gives up on its own schedule, which is tens of
// seconds: a board took 56 s to come back after a host restarted.
static int connect_to(struct sockaddr_in *host)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)host, sizeof(*host));
    if (rc != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        struct timeval timeout = {
            .tv_sec = CONNECT_TIMEOUT_MS / 1000,
            .tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000,
        };
        if (select(fd + 1, NULL, &writable, NULL, &timeout) <= 0) {
            close(fd);
            return -1;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
        if (error != 0) {
            close(fd);
            return -1;
        }
    }

    fcntl(fd, F_SETFL, flags);   // reads and writes stay blocking
    return fd;
}

// Reads until the host goes away. Lines arrive split across packets, so a
// partial tail has to survive into the next read.
static void pump(int fd)
{
    char pending[256];
    size_t used = 0;

    // The host speaks rarely, so a quiet socket is normal and a read timeout
    // cannot mean gone. It exists so this loop wakes often enough to notice the
    // writer having dropped the socket after a failed send.
    struct timeval read_timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));

    while (1) {
        char chunk[256];
        int n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) return;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!link_is_wireless()) return;
                continue;
            }
            return;
        }

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

// Where it looked last, then who answers. Remembering first means switching the
// game host off and on again costs a second rather than a discovery round.
static bool find_host(struct sockaddr_in *host, uint16_t *port)
{
    if (s_host[0] != '\0') {
        memset(host, 0, sizeof(*host));
        host->sin_family = AF_INET;
        host->sin_addr.s_addr = inet_addr(s_host);
        *port = s_host_port;
        if (host->sin_addr.s_addr != INADDR_NONE) return true;
    }
    return ask_for_host(host, port);
}

static void host_task(void *arg)
{
    bool remembered_failed = false;

    while (1) {
        xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        struct sockaddr_in host;
        uint16_t port = 0;

        // A remembered address that will not answer must not be tried for ever,
        // so every other round asks instead.
        bool ask_instead = remembered_failed;
        bool found = ask_instead ? ask_for_host(&host, &port) : find_host(&host, &port);
        if (!found) {
            remembered_failed = !remembered_failed;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        host.sin_port = htons(port);

        int fd = connect_to(&host);
        if (fd < 0) {
            // Say so once per gap rather than every retry: a controller that
            // goes quiet for half a minute should be able to tell you why, and
            // a line a second would bury the reason it started.
            if (!remembered_failed) {
                link_sendf("#net " IPSTR ":%u did not answer, looking again",
                           IP2STR((esp_ip4_addr_t *)&host.sin_addr), port);
                hud_set_link("looking for game");
            }
            remembered_failed = true;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        remembered_failed = false;

        char found_ip[16];
        snprintf(found_ip, sizeof(found_ip), IPSTR, IP2STR((esp_ip4_addr_t *)&host.sin_addr));
        remember_host(found_ip, port);

        // Samples are small and frequent, which is the traffic Nagle ruins.
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval send_timeout = {.tv_sec = 2};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

        // Samples ride a datagram to the port beside the socket's.
        struct sockaddr_in samples_to = host;
        samples_to.sin_port = htons(port + 1);
        int datagram = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        link_sendf("#net host %s:%u", found_ip, port);
        hud_set_link("in game");
        link_attach_socket(fd);
        if (datagram >= 0) link_attach_datagram(datagram, &samples_to);

        pump(fd);

        link_detach_datagram();
        link_detach_socket();
        if (datagram >= 0) close(datagram);
        close(fd);
        hud_set_link("looking for game");
        link_sendf("#net host gone, looking again");
    }
}

void net_add_network(const char *ssid, const char *password)
{
    int slot = s_network_count;
    for (int i = 0; i < s_network_count; i++) {
        if (strcmp(s_networks[i].ssid, ssid) == 0) slot = i;
    }
    // A board that has been everywhere forgets the oldest place first.
    if (slot == MAX_NETWORKS) {
        memmove(&s_networks[0], &s_networks[1], sizeof(network_t) * (MAX_NETWORKS - 1));
        slot = MAX_NETWORKS - 1;
    } else if (slot == s_network_count) {
        s_network_count++;
    }

    snprintf(s_networks[slot].ssid, sizeof(s_networks[slot].ssid), "%s", ssid);
    snprintf(s_networks[slot].password, sizeof(s_networks[slot].password), "%s", password);
    save_networks();

    link_sendf("#net know %d network%s", s_network_count, s_network_count == 1 ? "" : "s");
    esp_wifi_disconnect();
    xSemaphoreGive(s_rejoin);
}

void net_set_host(const char *ip, uint16_t port)
{
    remember_host(ip, port);
    link_sendf("#net host set to %s:%u", s_host, port);
}

esp_err_t net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    load_stored();
    if (s_migrated) save_networks();   // write the old pair of keys out in the new shape

    // A board that will not join is otherwise indistinguishable from a board
    // that has been told nothing, so say which it is before trying anything.
    link_sendf("#net know %d network%s%s", s_network_count,
               s_network_count == 1 ? "" : "s",
               s_migrated ? " (carried over from the old layout)" : "");
    if (s_host[0]) link_sendf("#net remember host %s:%u", s_host, s_host_port);

    s_events = xEventGroupCreate();
    s_rejoin = xSemaphoreCreateBinary();
    if (!s_events || !s_rejoin) return ESP_ERR_NO_MEM;

    if (xTaskCreate(join_task, "net_join", 5120, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

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
    ESP_ERROR_CHECK(esp_wifi_start());

    // Modem power save parks the radio between beacons: ICMP to this board sat
    // at a 6 ms floor with a 105 ms average, and the stick arrived in bursts
    // instead of a stream. It has to be set after the driver is started to
    // stick. A controller runs for one session, so the power is not worth it.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // A narrower channel is more robust in congestion than a wider one, and
    // this link carries a few KB/s, so there is no throughput to trade away.
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(80);

    if (s_network_count == 0) {
        link_sendf("#net no network stored; send: !wifi ssid=NAME pass=SECRET");
        hud_set_link("no wifi set");
    }

    return xTaskCreate(host_task, "net_host", 4096, NULL, 4, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}
