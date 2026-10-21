/*
 * Platform interface. Each port (Linux host, bare-metal RV32, Guition ESP32-P4) implements
 * these; the emulator core is otherwise free of platform dependencies.
 */
#ifndef P4DOS_HAL_H
#define P4DOS_HAL_H

#include <stdint.h>

/* For large buffers that a port may put in slower external RAM (the board's PSRAM). */
#ifndef HAL_EXT_BSS
#define HAL_EXT_BSS
#endif

typedef struct {
    int year, month, day; /* 1980.., 1-12, 1-31 */
    int hour, min, sec;
} hal_rtc_t;

/* Sector-addressed storage backing an emulated drive. Sectors are 512 bytes. */
typedef struct blkdev {
    int (*read)(struct blkdev *d, uint32_t lba, uint32_t count, uint8_t *buf);
    int (*write)(struct blkdev *d, uint32_t lba, uint32_t count, const uint8_t *buf);
    void (*flush)(struct blkdev *d);
    uint32_t sectors;
    int readonly;
    void *ctx;
} blkdev_t;

uint64_t hal_time_ns(void);          /* monotonic */
void hal_idle(uint32_t ns);          /* sleep, returning early on input */
void hal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hal_rtc_get(hal_rtc_t *t);
void hal_rtc_set(const hal_rtc_t *t);
void hal_lpt_putc(uint8_t c);        /* LPT1 output sink */
/* Called when the emulated PC (re)boots: attach or detach removable media (pc_attach). */
void hal_disks_refresh(void);
/* Guards the input queues, which other threads/tasks feed. No-ops on single-threaded ports. */
void hal_input_lock(void);
void hal_input_unlock(void);

#endif
