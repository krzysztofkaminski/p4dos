/*
 * Guition JC8012P4A1C-I-W-Y board definitions (see guition.md) and the interfaces
 * between the firmware modules.
 */
#ifndef P4DOS_BOARD_H
#define P4DOS_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal.h"
#include "sdkconfig.h"

/* Display: JD9365, MIPI-DSI, 2 lanes, 800x1280 portrait glass */
#define PIN_LCD_RST 27
#define PIN_LCD_BL 23 /* active high */
#define LCD_H_RES 800
#define LCD_V_RES 1280
#define DSI_LANE_MBPS 1500
#define DSI_PHY_LDO_CHAN 3 /* P4 internal LDO feeding the MIPI D-PHY */
#define DSI_PHY_LDO_MV 2500

/* Shared I2C bus: RX8025T RTC (0x32), plus touch and codec, which are not used here */
#define PIN_I2C_SDA 7
#define PIN_I2C_SCL 8
#define RTC_I2C_ADDR 0x32
#define CODEC_I2C_ADDR 0x18

/* Audio: ES8311 codec on I2S, NS4150 speaker amplifier */
#define PIN_I2S_MCLK 13
#define PIN_I2S_BCLK 12
#define PIN_I2S_WS 10
#define PIN_I2S_DOUT 9 /* data to the codec */
#define PIN_AMP_EN 20

/* TF card: SDMMC 4-bit, powered from internal LDO channel 4 */
#define PIN_SD_CLK 43
#define PIN_SD_CMD 44
#define PIN_SD_D0 39
#define PIN_SD_D1 40
#define PIN_SD_D2 41
#define PIN_SD_D3 42
#define SD_LDO_CHAN 4

esp_err_t display_init(void);
void display_start(void);          /* starts the refresh task */
#if CONFIG_P4DOS_STATS
extern volatile uint32_t display_frames, display_us; /* refreshes drawn, time they took */
extern volatile bool display_paused;
#endif

esp_err_t usbhid_start(void);

void storage_init(void);           /* attaches the drives to the emulator */
void storage_tick(void);           /* periodic write-back, emulator task */

void console_start(void);

void rtc_init(void);
struct i2c_master_bus_t *board_i2c_bus(void); /* the shared bus, NULL if it failed */

esp_err_t sound_start(void);

void p4dos_wake(void);           /* wake the emulator task (new input) */

#endif
