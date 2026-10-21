/*
 * DOS console on the USB-Serial-JTAG port (the upper USB-C, also used for flashing): the
 * text screen is mirrored to an ANSI terminal and its keystrokes go to the emulated
 * keyboard. Handy without a USB keyboard, and the first thing to look at when bringing the
 * board up. The same line carries the host disk (core/hostdisk.c).
 */
#include "board.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hostdisk.h"
#include "pc.h"
#include "term.h"

static const char *TAG = "console";

/* The terminal mirror and the frames of the host disk share the line. */
static SemaphoreHandle_t s_line, s_answer;
static bool s_repaint;

static int console_out(const char *buf, int len)
{
    /* A short timeout: when nothing reads the port the data is dropped, and the mirror
     * is told so that it repaints once a terminal is listening. */
    xSemaphoreTake(s_line, portMAX_DELAY);
    const int sent = usb_serial_jtag_write_bytes(buf, (size_t)len, pdMS_TO_TICKS(20));
    xSemaphoreGive(s_line);
    return sent != len;
}

static int frame_out(const void *buf, int len)
{
    const char *p = buf;
    int lost = 0;
    xSemaphoreTake(s_line, portMAX_DELAY);
    while (len > 0 && !lost) {
        const int n = len > 2048 ? 2048 : len;
        lost = usb_serial_jtag_write_bytes(p, (size_t)n, pdMS_TO_TICKS(500)) != n;
        p += n;
        len -= n;
    }
    xSemaphoreGive(s_line);
    return lost;
}

static void answer_wait(int ms)
{
    xSemaphoreTake(s_answer, pdMS_TO_TICKS(ms) + 1);
}

static void answer_wake(void)
{
    xSemaphoreGive(s_answer);
}

static void console_key(int ch)
{
    if (ch < 0) {
        kbd_term_char(-1); /* input went quiet: a lone ESC was the Esc key */
        return;
    }
#if CONFIG_P4DOS_STATS
    /* Ctrl-^ and two hex digits: a scancode as it is; Ctrl-^ D: display on/off */
    static int raw = -1, raw_hi;
    if (raw >= 0) {
        const int c = ch, d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
        if (raw == 0 && c == 'D') {
            display_paused = !display_paused;
            raw = -1;
        } else if (d < 0) {
            raw = -1;
        } else if (raw == 0) {
            raw_hi = d;
            raw = 1;
        } else {
            kbd_push((uint8_t)(raw_hi * 16 + d));
            raw = -1;
        }
        return;
    }
    if (ch == 0x1E) {
        raw = 0;
        return;
    }
#endif
    if (ch == 0x0C) { /* Ctrl-L: repaint the terminal */
        s_repaint = true;
    } else {
        kbd_term_char(ch);
    }
}

static const hostdisk_port_t s_hostdisk = {
    .send = frame_out, .key = console_key, .wait = answer_wait, .wake = answer_wake};

static void console_task(void *arg)
{
    bool connected = false, pending_esc = false;
    TickType_t last_refresh = 0, interval = pdMS_TO_TICKS(50);
    (void)arg;

    term_init(console_out);
    for (;;) {
        uint8_t buf[256];
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(40));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                hostdisk_input(buf[i]);
            }
            pending_esc = true;
            p4dos_wake();
        } else if (pending_esc) {
            hostdisk_input(-1);
            pending_esc = false;
            p4dos_wake();
        }

        bool now_connected = usb_serial_jtag_is_connected();
        TickType_t now = xTaskGetTickCount();
        if (now_connected && now - last_refresh >= interval) {
            last_refresh = now;
            /* Full repaint when a host appears or on request. If the output could not be
             * delivered (no program has the port open), retry less often. */
            bool failed = term_refresh(!connected || s_repaint);
            interval = pdMS_TO_TICKS(failed ? 500 : 50);
            s_repaint = false;
        }
        connected = now_connected;
    }
}

void console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 8192;
    /* Nothing holds the host back when this is full: room for a whole frame of the host
     * disk, which is the most it sends unasked. */
    cfg.rx_buffer_size = 16384;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB-Serial-JTAG driver unavailable; no serial console");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    s_line = xSemaphoreCreateMutex();
    s_answer = xSemaphoreCreateBinary();
    hostdisk_init(&s_hostdisk);
    /* From here on the port shows the DOS screen; keep the log to real problems, plus the
     * occasional line about a USB device coming or going or the volume changing. */
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("usbhid", ESP_LOG_INFO);
    esp_log_level_set("sound", ESP_LOG_INFO);
    xTaskCreatePinnedToCore(console_task, "console", 6144, NULL, 3, NULL, 0);
}
