#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "link.h"

#define LINE_MAX 256

static link_line_cb_t s_on_line;
static SemaphoreHandle_t s_lock;
static int s_socket = -1;   // -1 means the line runs over USB

// One send() per sample is slower than the sample rate over wifi, and because
// the sampling loop does the sending, the whole controller drops to whatever
// the radio can manage: 200 Hz became 70. Lines are queued here instead and a
// writer drains them, so the loop never waits on the network and whatever
// accumulates while a send is in flight goes out as one packet. Latency stays
// low when idle because the writer is woken per line, not on a timer.
#define OUT_MAX 4096

// Minimum spacing between transmissions, which sets how much each one carries.
#define SEND_GAP_MS 40
static char s_out[OUT_MAX];
static size_t s_out_used;
static uint32_t s_dropped;
static SemaphoreHandle_t s_wake;

// stdin over USB CDC only blocks properly once the driver is installed and the
// VFS is pointed at it. Without this the reader spins on EOF and nothing typed
// at the board ever arrives.
static void usb_reader_task(void *arg)
{
    char line[LINE_MAX];
    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0') link_deliver(line);
    }
}

// Sends whatever has piled up since the last write, in one call.
static void writer_task(void *arg)
{
    static char batch[OUT_MAX];
    while (1) {
        xSemaphoreTake(s_wake, portMAX_DELAY);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t length = s_out_used;
        int fd = s_socket;
        if (length) {
            memcpy(batch, s_out, length);
            s_out_used = 0;
        }
        xSemaphoreGive(s_lock);

        if (!length || fd < 0) continue;

        // A wedged host must not back up the queue for ever. Dropping the
        // socket hands it to the reconnect loop and the line falls back to USB.
        if (send(fd, batch, length, 0) < 0) {
            link_detach_socket();
            continue;
        }

        // What a congested channel charges for is packets, not bytes: each one
        // contends for airtime and may be retried. Holding off briefly after a
        // send makes the next one carry everything that piled up meanwhile, so
        // the stick costs a quarter of the transmissions it used to. The delay
        // is answering a link whose round trip is already hundreds of
        // milliseconds, so it is not what anyone will feel.
        vTaskDelay(pdMS_TO_TICKS(SEND_GAP_MS));
    }
}

esp_err_t link_start(link_line_cb_t on_line)
{
    s_on_line = on_line;
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (!s_lock || !s_wake) return ESP_ERR_NO_MEM;

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) return err;
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);

    if (xTaskCreate(writer_task, "link_tx", 4096, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(usb_reader_task, "link_usb", 4096, NULL, 5, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
}

void link_deliver(const char *line)
{
    if (s_on_line) s_on_line(line);
}

void link_attach_socket(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_socket = fd;
    xSemaphoreGive(s_lock);
}

void link_detach_socket(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_socket = -1;
    xSemaphoreGive(s_lock);
}

bool link_is_wireless(void)
{
    return s_socket >= 0;
}

void link_sendf(const char *fmt, ...)
{
    char line[LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (n <= 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool wireless = s_socket >= 0;
    if (wireless) {
        if (s_out_used + n <= OUT_MAX) {
            memcpy(s_out + s_out_used, line, n);
            s_out_used += n;
        } else {
            // Newest samples are the ones worth having, but a queue that
            // overflows silently is a link that lies about its rate, so the
            // loss is counted and reported in the banner.
            s_dropped++;
        }
    }
    xSemaphoreGive(s_lock);

    if (wireless) xSemaphoreGive(s_wake);
    else fwrite(line, 1, n, stdout);
}

uint32_t link_dropped(void)
{
    return s_dropped;
}
