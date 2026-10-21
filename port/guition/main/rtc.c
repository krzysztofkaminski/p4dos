/*
 * Real-time clock for the BIOS (INT 1Ah). Uses the board's RX8025T on the shared I2C
 * bus when it answers with a valid time; otherwise a software clock that starts at a
 * fixed date and can be set from DOS (DATE / TIME) for the session.
 */
#include <time.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "rtc";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_hw_ok;
static time_t s_soft_base = 1767225600; /* 2026-01-01 00:00:00 */

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

static bool hw_read(hal_rtc_t *t)
{
    uint8_t reg = 0x00, d[7], flag_reg = 0x0E, flags = 0;
    if (!s_dev) {
        return false;
    }
    if (i2c_master_transmit_receive(s_dev, &flag_reg, 1, &flags, 1, 50) != ESP_OK ||
        (flags & 0x02)) { /* VLF: the clock lost power, its contents are not valid */
        return false;
    }
    if (i2c_master_transmit_receive(s_dev, &reg, 1, d, sizeof(d), 50) != ESP_OK) {
        return false;
    }
    t->sec = from_bcd(d[0] & 0x7F);
    t->min = from_bcd(d[1] & 0x7F);
    t->hour = from_bcd(d[2] & 0x3F);
    t->day = from_bcd(d[4] & 0x3F);
    t->month = from_bcd(d[5] & 0x1F);
    t->year = 2000 + from_bcd(d[6]);
    return t->sec < 60 && t->min < 60 && t->hour < 24 && t->day >= 1 && t->day <= 31 &&
           t->month >= 1 && t->month <= 12;
}

void rtc_init(void)
{
    hal_rtc_t t;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* The board has external pull-ups on this bus; the internal ones must stay off. */
        .flags.enable_internal_pullup = false,
    };
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        s_bus = NULL;
    } else if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
    }
    s_hw_ok = hw_read(&t);
    if (s_hw_ok) {
        ESP_LOGI(TAG, "RX8025T: %04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour,
                 t.min, t.sec);
    } else {
        ESP_LOGW(TAG, "RX8025T not usable (absent, or no backup cell); using a software clock");
    }
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

void hal_rtc_get(hal_rtc_t *t)
{
    if (s_hw_ok && hw_read(t)) {
        return;
    }
    time_t now = s_soft_base + (time_t)(esp_timer_get_time() / 1000000);
    struct tm tm;
    gmtime_r(&now, &tm);
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day = tm.tm_mday;
    t->hour = tm.tm_hour;
    t->min = tm.tm_min;
    t->sec = tm.tm_sec;
}

void hal_rtc_set(const hal_rtc_t *t)
{
    struct tm tm = {
        .tm_year = t->year - 1900,
        .tm_mon = t->month - 1,
        .tm_mday = t->day,
        .tm_hour = t->hour,
        .tm_min = t->min,
        .tm_sec = t->sec,
    };
    time_t when = mktime(&tm); /* no TZ is set: local time is UTC */
    s_soft_base = when - (time_t)(esp_timer_get_time() / 1000000);

    if (s_dev) {
        uint8_t w[8] = {0x00,
                        to_bcd(t->sec),
                        to_bcd(t->min),
                        to_bcd(t->hour),
                        (uint8_t)(1u << tm.tm_wday),
                        to_bcd(t->day),
                        to_bcd(t->month),
                        to_bcd(t->year % 100)};
        uint8_t clear_flags[2] = {0x0E, 0x00};
        if (i2c_master_transmit(s_dev, w, sizeof(w), 50) == ESP_OK &&
            i2c_master_transmit(s_dev, clear_flags, sizeof(clear_flags), 50) == ESP_OK) {
            s_hw_ok = true;
        }
    }
}
