/*
 * Panel bring-up (JD9365 over MIPI-DSI) and the refresh task.
 *
 * The emulator's renderer draws straight into the DPI frame buffer in PSRAM, already
 * rotated for the portrait glass, and only where the picture changed. The touched lines are
 * then handed to the panel driver, which for a pointer inside its own frame buffer just
 * writes the cache back so the DSI scan-out sees them.
 */
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jd9365_init.h"
#include "jd9365_init_new.h"
#include "sdkconfig.h"
#include "video.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
#if CONFIG_P4DOS_STATS
volatile uint32_t display_frames, display_us;
volatile bool display_paused;
#endif

esp_err_t display_init(void)
{
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
    gpio_set_level(PIN_LCD_BL, 0); /* dark until the first frame is in place */

    /* The MIPI D-PHY runs off an internal LDO that is off by default. */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "dphy ldo");

    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
#if CONFIG_P4DOS_LCD_NEW_PANEL
    bus_cfg.lane_bit_rate_mbps = 840; /* the vendor's ESP-IDF 5.5.4 example for this panel */
#else
    bus_cfg.lane_bit_rate_mbps = DSI_LANE_MBPS;
#endif
    /* ESP-IDF's default PLL reference for the chip revision built for: the macro's
     * (PLL_F20M) does not exist from revision v3.0 on. */
    bus_cfg.phy_clk_src = 0;
    esp_lcd_dsi_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "dbi io");

    /* Timings for this glass (guition.md): 60 MHz pixel clock; the generic JD9365
     * 70-80 MHz settings collapse the image into a corner. The new panel's are those of
     * the vendor's example for it. One frame buffer. */
    esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.num_fbs = 1;
    dpi_cfg.video_timing.hsync_back_porch = 20;
    dpi_cfg.video_timing.hsync_pulse_width = 20;
    dpi_cfg.video_timing.hsync_front_porch = 40;
    dpi_cfg.video_timing.vsync_pulse_width = 4;
#if CONFIG_P4DOS_LCD_NEW_PANEL
    dpi_cfg.dpi_clock_freq_mhz = 70;
    dpi_cfg.video_timing.vsync_back_porch = 10;
    dpi_cfg.video_timing.vsync_front_porch = 30;
    static const jd9365_lcd_init_cmd_t *const init = guition_jd9365_init_new;
    const uint16_t init_n = sizeof(guition_jd9365_init_new) / sizeof(guition_jd9365_init_new[0]);
#else
    dpi_cfg.dpi_clock_freq_mhz = 60;
    dpi_cfg.video_timing.vsync_back_porch = 8;
    dpi_cfg.video_timing.vsync_front_porch = 20;
    static const jd9365_lcd_init_cmd_t *const init = guition_jd9365_init;
    const uint16_t init_n = sizeof(guition_jd9365_init) / sizeof(guition_jd9365_init[0]);
#endif

    jd9365_vendor_config_t vendor_cfg = {
        .init_cmds = init,
        .init_cmds_size = init_n,
        .mipi_config = {
            .dsi_bus = bus,
            .dpi_config = &dpi_cfg,
            .lane_num = 2,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(io, &panel_cfg, &s_panel), TAG, "new jd9365");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb), TAG, "frame buffer");
    s_fb = fb;
    memset(s_fb, 0, (size_t)LCD_H_RES * LCD_V_RES * 2);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
    ESP_LOGI(TAG, "JD9365 %dx%d up, used as %dx%d landscape", LCD_H_RES, LCD_V_RES, SCREEN_W,
             SCREEN_H);
    return ESP_OK;
}

static void display_task(void *arg)
{
    const render_target_t target = {
        .fb = s_fb,
        .stride = LCD_H_RES,
#if CONFIG_P4DOS_ROTATE_180
        .rotate = ROT_CCW,
#else
        .rotate = ROT_CW,
#endif
    };
    bool lit = false;
    TickType_t wake = xTaskGetTickCount();
    (void)arg;

    for (;;) {
        int row0 = 0, row1 = 0;
#if CONFIG_P4DOS_STATS
        const int64_t t0 = esp_timer_get_time();
        if (display_paused) {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(25));
            continue;
        }
#endif
        if (render_update(&target, (uint64_t)esp_timer_get_time() * 1000u, 0, &row0, &row1)) {
            /* The pointer lies inside the panel's frame buffer, so this only syncs the
             * cache for lines row0..row1. */
            esp_lcd_panel_draw_bitmap(s_panel, 0, row0, LCD_H_RES, row1 + 1,
                                      s_fb + (size_t)row0 * LCD_H_RES);
#if CONFIG_P4DOS_STATS
            display_frames++;
            display_us += (uint32_t)(esp_timer_get_time() - t0);
#endif
        }
        if (!lit) {
            lit = true;
            gpio_set_level(PIN_LCD_BL, 1);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(25));
    }
}

void display_start(void)
{
    xTaskCreatePinnedToCore(display_task, "display", 8192, NULL, 4, NULL, 0);
}
