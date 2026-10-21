/*
 * P4DOS firmware entry: brings the board up, creates the emulated PC and runs it.
 *
 *   core 1: the emulator task (CPU interpreter + device models)
 *   core 0: display refresh, USB host, serial console
 */
#include <stdarg.h>
#include <stdio.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pc.h"
#include "sdkconfig.h"
#include "video.h"

static const char *TAG = "p4dos";

static TaskHandle_t s_emu_task;

static portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;

/* --- HAL -------------------------------------------------------------------- */

uint64_t hal_time_ns(void)
{
    return (uint64_t)esp_timer_get_time() * 1000u;
}

void hal_idle(uint32_t ns)
{
    if (ns >= 2000000u) {
        /* Sleep, but wake a tick early: the rest is waited out below on the next call. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ns / 1000000u) - 1);
        return;
    }
    /* Close to a timer deadline. FreeRTOS cannot sleep for less than a tick, and the
     * emulated timer interrupt has to arrive on time, so spin for the last stretch. */
    int64_t end = esp_timer_get_time() + ns / 1000;
    while (esp_timer_get_time() < end) {
        if (ulTaskNotifyTake(pdTRUE, 0)) {
            break;
        }
    }
}

void hal_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    esp_log_writev(ESP_LOG_WARN, TAG, fmt, ap);
    va_end(ap);
}

void hal_lpt_putc(uint8_t c)
{
    (void)c; /* no printer */
}

void hal_input_lock(void)
{
    portENTER_CRITICAL(&s_input_mux);
}

void hal_input_unlock(void)
{
    portEXIT_CRITICAL(&s_input_mux);
}

void p4dos_wake(void)
{
    if (s_emu_task) {
        xTaskNotifyGive(s_emu_task);
    }
}

/* --- emulator --------------------------------------------------------------- */

#if CONFIG_P4DOS_STATS
/* Where the guest is, sampled a thousand times a second from the other core. */
#define PROF_SLOTS 1024
static struct {
    uint32_t key, n;
} s_prof[PROF_SLOTS];
static volatile uint32_t s_prof_halted, s_prof_total;

static void prof_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    (void)arg;
    for (;;) {
        /* code segment and 64-byte block of the instruction being executed */
        const uint32_t key = ((uint32_t)cpu.seg[SEG_CS] << 20) ^ (cpu.insn_ip >> 6) ^ ((uint32_t)(cpu.cr0 & 1) << 31);
        uint32_t h = (key * 2654435761u) >> 22;
        s_prof_total++;
        if (cpu.halted) {
            s_prof_halted++;
        } else {
            for (int i = 0; i < 16; i++, h = (h + 1) % PROF_SLOTS) {
                if (s_prof[h].n == 0 || s_prof[h].key == key) {
                    s_prof[h].key = key;
                    s_prof[h].n++;
                    break;
                }
            }
        }
        vTaskDelayUntil(&wake, 1);
    }
}

static void prof_report(void)
{
    char line[200];
    int len = 0;
    const uint32_t total = s_prof_total ? s_prof_total : 1;
    len += snprintf(line + len, sizeof(line) - (size_t)len, "halted %u%%;", (unsigned)(s_prof_halted * 100 / total));
    for (int k = 0; k < 9; k++) {
        int best = -1;
        for (int i = 0; i < PROF_SLOTS; i++) {
            if (s_prof[i].n && (best < 0 || s_prof[i].n > s_prof[best].n)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %s%03X:%06X %u%%", (s_prof[best].key >> 31) ? "" : "r",
                        (unsigned)((s_prof[best].key >> 20) & 0x7FF), (unsigned)((s_prof[best].key & 0xFFFFF) << 6),
                        (unsigned)(s_prof[best].n * 100 / total));
        s_prof[best].n = 0;
    }
    for (int i = 0; i < PROF_SLOTS; i++) {
        s_prof[i].n = 0;
    }
    s_prof_total = s_prof_halted = 0;
    ESP_LOGW(TAG, "prof: %s", line);
}
#endif

static void emu_task(void *arg)
{
    int64_t last_tick = 0;
    (void)arg;
    for (;;) {
        for (int i = 0; i < 16; i++) {
            pc_step();
        }
        int64_t now = esp_timer_get_time();
        if (now - last_tick > 500000) {
            last_tick = now;
            storage_tick();
        }
#if CONFIG_P4DOS_STATS
        {
            static int64_t stat_t;
            static uint64_t stat_n;
            static uint32_t stat_f, stat_us;
            if (now - stat_t >= 2000000) {
                const uint32_t f = display_frames, us = display_us;
                if (stat_t) {
                    const uint32_t kips = (uint32_t)((cpu.icount - stat_n) * 1000000ull / (uint64_t)(now - stat_t) / 1000u);
                    ESP_LOGW(TAG, "stats: %u.%03u MIPS, %u M instructions; display %u frames, %u ms; at %04X:%08X",
                             (unsigned)(kips / 1000), (unsigned)(kips % 1000), (unsigned)(cpu.icount / 1000000u),
                             (unsigned)(f - stat_f), (unsigned)((us - stat_us) / 1000u), cpu.seg[SEG_CS],
                             (unsigned)cpu.ip);
                    prof_report();
                }
                stat_t = now;
                stat_n = cpu.icount;
                stat_f = f;
                stat_us = us;
            }
        }
#endif
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "P4DOS %s starting", P4DOS_VERSION);

    pc_config_t cfg = {
#if CONFIG_P4DOS_CPU_386
        .cpu_model = CPU_80386,
        .ram_kb = CONFIG_P4DOS_RAM_MB * 1024,
#if CONFIG_P4DOS_FPU
        .fpu = 1,
#endif
#elif CONFIG_P4DOS_CPU_186
        .cpu_model = CPU_80186,
        .ram_kb = 1024,
#else
        .cpu_model = CPU_8086,
        .ram_kb = 1024,
#endif
        .ems_kb = CONFIG_P4DOS_EMS_KB,
    };
    /* Take what PSRAM gives: less extended memory is better than no machine. */
    while (!(cfg.ram = heap_caps_malloc((size_t)cfg.ram_kb * 1024 + 16, MALLOC_CAP_SPIRAM))) {
        if (cfg.ram_kb <= 1024) {
            ESP_LOGE(TAG, "no memory for the guest RAM; is PSRAM working?");
            return;
        }
        cfg.ram_kb = cfg.ram_kb > 4096 ? cfg.ram_kb - 4096 : 1024;
    }
    ESP_LOGI(TAG, "guest RAM: %u KB", (unsigned)cfg.ram_kb);
    cfg.vram = heap_caps_malloc(VGA_RAM_SIZE, MALLOC_CAP_SPIRAM);
    if (!cfg.vram) {
        ESP_LOGE(TAG, "no memory for the video RAM");
        return;
    }
    if (cfg.ems_kb) {
        cfg.ems_ram = heap_caps_malloc((size_t)cfg.ems_kb * 1024, MALLOC_CAP_SPIRAM);
        if (!cfg.ems_ram) {
            ESP_LOGW(TAG, "no memory for %u KB of EMS; continuing without", (unsigned)cfg.ems_kb);
            cfg.ems_kb = 0;
        }
    }
    /* On the 80386 extended memory is part of the guest RAM. */
#ifdef CONFIG_P4DOS_XMS_KB
    cfg.xms_kb = CONFIG_P4DOS_XMS_KB;
#endif
    if (cfg.xms_kb) {
        cfg.xms_ram = heap_caps_malloc((size_t)cfg.xms_kb * 1024, MALLOC_CAP_SPIRAM);
        if (!cfg.xms_ram) {
            ESP_LOGW(TAG, "no memory for %u KB of XMS; continuing without", (unsigned)cfg.xms_kb);
            cfg.xms_kb = 0;
        }
    }

    rtc_init();
    pc_init(&cfg);
    storage_init();
    pc_reset();

    /* Everything below is optional hardware: a failure is logged and the machine still
     * runs, at worst on the serial console only. */
    esp_err_t err = display_init();
    if (err == ESP_OK) {
        display_start();
    } else {
        ESP_LOGE(TAG, "display init failed (%s); running without the panel", esp_err_to_name(err));
    }
    err = usbhid_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host init failed (%s); no USB keyboard", esp_err_to_name(err));
    }
#if CONFIG_P4DOS_SOUND
    err = sound_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sound init failed (%s); running silent", esp_err_to_name(err));
    }
#endif
#if CONFIG_P4DOS_SERIAL_CONSOLE
    console_start();
#endif

    xTaskCreatePinnedToCore(emu_task, "emu", 16384, NULL, 5, &s_emu_task, 1);
#if CONFIG_P4DOS_STATS
    xTaskCreatePinnedToCore(prof_task, "prof", 3072, NULL, 7, NULL, 0);
#endif
    ESP_LOGI(TAG, "emulator running; heap free: %u KB internal, %u KB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
