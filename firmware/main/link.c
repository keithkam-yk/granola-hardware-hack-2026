#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "link.h"

#define LINE_MAX 256

static link_line_cb_t s_on_line;

// stdin over USB CDC only blocks properly once the driver is installed and the
// VFS is pointed at it. Without this the reader spins on EOF and the host can
// never talk back.
static void reader_task(void *arg)
{
    char line[LINE_MAX];
    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] != '\0' && s_on_line) s_on_line(line);
    }
}

esp_err_t link_start(link_line_cb_t on_line)
{
    s_on_line = on_line;

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) return err;
    usb_serial_jtag_vfs_use_driver();

    setvbuf(stdin, NULL, _IONBF, 0);

    return xTaskCreate(reader_task, "link_rx", 4096, NULL, 5, NULL) == pdPASS
           ? ESP_OK : ESP_ERR_NO_MEM;
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
    fwrite(line, 1, n, stdout);
}
