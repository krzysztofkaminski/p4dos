/*
 * Audio output: the emulator's mixer (Sound Blaster and PC speaker) is played through the
 * board's ES8311 codec over I2S, with the NS4150 speaker amplifier switched by GPIO20.
 * guition.md lists this hardware as taken from the vendor documents and not exercised, so
 * every step here may fail softly and leave the machine silent.
 */
#include "audio.h"
#include "board.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "sound";

static esp_codec_dev_handle_t s_dev;
static bool s_nvs_ok;

/* The volume chosen with Ctrl+Alt+Up/Down is kept across power cycles. The NVS partition
 * may hold another firmware's settings: it is never erased here, only added to. */
static void volume_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_flash_init() != ESP_OK) {
        ESP_LOGW(TAG, "NVS not usable; the volume setting will not be remembered");
        return;
    }
    s_nvs_ok = true;
    /* Boards in use have their setting under this namespace. */
    if (nvs_open("gdos", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "volume", &v) == ESP_OK) {
            audio_set_volume(v);
        }
        nvs_close(h);
    }
}

static void volume_save(int v)
{
    nvs_handle_t h;
    if (s_nvs_ok && nvs_open("gdos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "volume", (uint8_t)v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void sound_task(void *arg)
{
    static int16_t buf[256];
    int shown = audio_get_volume(), saved = shown;
    TickType_t changed = 0;
    (void)arg;
    for (;;) {
        int v = audio_get_volume();
        if (v != shown) {
            shown = v;
            changed = xTaskGetTickCount();
            ESP_LOGI(TAG, "volume %d%%", v);
        }
        if (shown != saved && xTaskGetTickCount() - changed > pdMS_TO_TICKS(2000)) {
            saved = shown; /* written once the setting has settled, to spare the flash */
            volume_save(saved);
        }
        audio_render(buf, 256);
        if (esp_codec_dev_write(s_dev, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(20)); /* never spin if the I2S path is unhappy */
        }
    }
}

esp_err_t sound_start(void)
{
    volume_load();
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    if (i2c_master_probe(bus, CODEC_I2C_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no ES8311 codec at I2C address 0x%02x", CODEC_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = tx,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!ctrl_if || !data_if || !gpio_if) {
        return ESP_FAIL;
    }
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PIN_AMP_EN,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) {
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_dev, CONFIG_P4DOS_VOLUME);

    /* Above the display task: a late audio block is heard, a late frame is not seen. */
    if (xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 6, NULL, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8311 codec up: %d Hz mono, codec level %d, volume %d%%", AUDIO_RATE,
             CONFIG_P4DOS_VOLUME, audio_get_volume());
    return ESP_OK;
}
