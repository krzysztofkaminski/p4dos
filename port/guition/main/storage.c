/*
 * Drives of the emulated PC.
 *
 *   C: (hard disk 0)  the "dosdisk" flash partition, holding the FreeDOS image
 *   D: (hard disk 1)  the TF card itself: DOS reads its partition table and FAT16/FAT32
 *                     file system directly, so the card moves between a PC and the board
 *
 * Alternatively (P4DOS_SD_IMAGES) the card is mounted by the firmware and holds disk image
 * files: /P4DOS/A.IMG and /P4DOS/B.IMG as floppy drives, /P4DOS/HD.IMG as D:.
 *
 * NOR flash is erased in 4 KiB blocks, so sector writes to C: go through a small
 * write-back cache: a block is rewritten only when its contents really changed, erased
 * only when bits must go from 0 to 1, and committed when the guest goes idle (and at
 * least twice a second). All of this runs in the emulator task.
 */
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "pc.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

/* --- flash partition disk ----------------------------------------------------- */

#define BLK 4096u
#define SEC_PER_BLK (BLK / 512u)
#define NCACHE 8

typedef struct {
    int32_t blk; /* -1 = unused */
    uint8_t dirty;
    uint32_t used;
    uint8_t data[BLK];
} centry_t;

static const esp_partition_t *s_part;
static centry_t *s_cache;
static uint8_t *s_old;
static uint32_t s_use_counter;
static int s_dirty;

static int commit(centry_t *e)
{
    size_t off = (size_t)e->blk * BLK;
    int need_erase = 0;

    if (esp_partition_read(s_part, off, s_old, BLK) != ESP_OK) {
        return -1;
    }
    e->dirty = 0;
    if (!memcmp(s_old, e->data, BLK)) {
        return 0;
    }
    for (unsigned i = 0; i < BLK; i++) {
        if ((s_old[i] & e->data[i]) != e->data[i]) {
            need_erase = 1;
            break;
        }
    }
    if (need_erase && esp_partition_erase_range(s_part, off, BLK) != ESP_OK) {
        return -1;
    }
    return esp_partition_write(s_part, off, e->data, BLK) == ESP_OK ? 0 : -1;
}

static centry_t *lookup(int32_t blk)
{
    for (int i = 0; i < NCACHE; i++) {
        if (s_cache[i].blk == blk) {
            return &s_cache[i];
        }
    }
    return NULL;
}

static centry_t *acquire(int32_t blk)
{
    centry_t *e = lookup(blk), *victim = &s_cache[0];
    if (!e) {
        for (int i = 0; i < NCACHE; i++) {
            if (s_cache[i].blk < 0) {
                victim = &s_cache[i];
                break;
            }
            if (s_cache[i].used < victim->used) {
                victim = &s_cache[i];
            }
        }
        if (victim->blk >= 0 && victim->dirty && commit(victim)) {
            return NULL;
        }
        if (esp_partition_read(s_part, (size_t)blk * BLK, victim->data, BLK) != ESP_OK) {
            victim->blk = -1;
            return NULL;
        }
        victim->blk = blk;
        victim->dirty = 0;
        e = victim;
    }
    e->used = ++s_use_counter;
    return e;
}

static int flash_read(blkdev_t *d, uint32_t lba, uint32_t n, uint8_t *buf)
{
    (void)d;
    if (esp_partition_read(s_part, (size_t)lba * 512, buf, (size_t)n * 512) != ESP_OK) {
        return -1;
    }
    /* Blocks waiting in the cache are newer than the flash contents. */
    for (uint32_t i = 0; i < n; i++) {
        centry_t *e = lookup((int32_t)((lba + i) / SEC_PER_BLK));
        if (e) {
            memcpy(buf + (size_t)i * 512, e->data + ((lba + i) % SEC_PER_BLK) * 512, 512);
        }
    }
    return 0;
}

static int flash_write(blkdev_t *d, uint32_t lba, uint32_t n, const uint8_t *buf)
{
    (void)d;
    for (uint32_t i = 0; i < n; i++) {
        centry_t *e = acquire((int32_t)((lba + i) / SEC_PER_BLK));
        if (!e) {
            return -1;
        }
        memcpy(e->data + ((lba + i) % SEC_PER_BLK) * 512, buf + (size_t)i * 512, 512);
        e->dirty = 1;
    }
    s_dirty = 1;
    return 0;
}

static void flash_flush(blkdev_t *d)
{
    (void)d;
    if (!s_dirty) {
        return;
    }
    s_dirty = 0;
    for (int i = 0; i < NCACHE; i++) {
        if (s_cache[i].blk >= 0 && s_cache[i].dirty && commit(&s_cache[i])) {
            ESP_LOGE(TAG, "flash write failed at block %d", (int)s_cache[i].blk);
        }
    }
}

static blkdev_t s_flash_dev = {.read = flash_read, .write = flash_write, .flush = flash_flush};

static void flash_disk_init(void)
{
    s_part = esp_partition_find_first(0x40, ESP_PARTITION_SUBTYPE_ANY, "dosdisk");
    if (!s_part) {
        ESP_LOGE(TAG, "no \"dosdisk\" partition: there will be no drive C:");
        return;
    }
    s_cache = heap_caps_malloc(sizeof(centry_t) * NCACHE, MALLOC_CAP_INTERNAL);
    s_old = heap_caps_malloc(BLK, MALLOC_CAP_INTERNAL);
    if (!s_cache || !s_old) {
        ESP_LOGE(TAG, "no memory for the flash disk cache");
        return;
    }
    for (int i = 0; i < NCACHE; i++) {
        s_cache[i].blk = -1;
        s_cache[i].dirty = 0;
        s_cache[i].used = 0;
    }
    s_flash_dev.sectors = s_part->size / 512;
    pc_attach(DRIVE_HD0, &s_flash_dev);
    ESP_LOGI(TAG, "C: = flash partition \"dosdisk\", %u KB", (unsigned)(s_part->size / 1024));
}

/* --- the TF card as a raw disk ------------------------------------------------- */

#if CONFIG_P4DOS_SD_CARD && CONFIG_P4DOS_SD_RAW

static sdmmc_card_t s_card;
static bool s_card_ok;
static blkdev_t s_card_dev;
static sd_pwr_ctrl_handle_t s_sd_pwr;

/* A failed transfer can leave the card stuck in the middle of it, and ESP-IDF does not get
 * it out of there. Seen on the rev v3.2 board: writes failing with CRC errors at 20 MHz,
 * then a multi-block write timing out with the card left in its receive state, after which
 * it refused every command until a power-on. So a failed transfer restarts the card in the
 * next of these modes and is tried again. On 2026-10-20 the same board's writes failed
 * with CRC errors at 5 MHz too, single sectors included, while reads and commands worked:
 * the data lines in the direction of the card, not the clock, so the fallback is the 1-bit
 * bus (D0 only). A card that does not take writes in the last mode becomes read-only, so
 * that DOS gets "write protected" at once instead of stalling for minutes over its own
 * retries (8 s per failed multi-sector write). */
static const struct {
    uint16_t khz;
    uint8_t width;
} s_card_modes[] = {{SDMMC_FREQ_DEFAULT, 4}, {SDMMC_FREQ_DEFAULT, 1}, {5000, 1}};
#define CARD_MODES (sizeof(s_card_modes) / sizeof(s_card_modes[0]))
static unsigned s_card_step;

static bool card_open(void);

static void card_restart(void)
{
    sdmmc_host_deinit();
    if (s_card_step + 1 < CARD_MODES) {
        s_card_step++;
    }
    s_card_ok = card_open();
    ESP_LOGW(TAG, "TF card restarted at %u kHz, %u-bit bus%s",
             (unsigned)s_card_modes[s_card_step].khz, (unsigned)s_card_modes[s_card_step].width,
             s_card_ok ? "" : ": failed");
}

static int card_io(bool write, uint32_t lba, uint32_t n, void *buf)
{
    for (int tries = 0; tries < 3 && s_card_ok; tries++) {
        const esp_err_t err = write ? sdmmc_write_sectors(&s_card, buf, lba, n)
                                    : sdmmc_read_sectors(&s_card, buf, lba, n);
        if (err == ESP_OK) {
            return 0;
        }
        ESP_LOGW(TAG, "TF card %s of %u sectors at %u: %s", write ? "write" : "read",
                 (unsigned)n, (unsigned)lba, esp_err_to_name(err));
        const bool last = s_card_step + 1 == CARD_MODES;
        card_restart();
        if (write && last) {
            s_card_dev.readonly = 1;
            ESP_LOGW(TAG, "TF card does not take writes: D: is read-only until the card is "
                          "changed or the board restarts");
            return -1;
        }
    }
    return -1;
}

static int card_read(blkdev_t *d, uint32_t lba, uint32_t n, uint8_t *buf)
{
    (void)d;
    return card_io(false, lba, n, buf);
}

static int card_write(blkdev_t *d, uint32_t lba, uint32_t n, const uint8_t *buf)
{
    (void)d;
    return card_io(true, lba, n, (void *)buf);
}

static blkdev_t s_card_dev = {.read = card_read, .write = card_write};

static bool card_open(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = s_card_modes[s_card_step].khz;

    /* An empty slot is normal and is probed at every reboot: no error lines for it. */
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_init", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
    esp_log_level_set("ldo", ESP_LOG_ERROR);

    /* The slot is powered from the P4's internal LDO channel 4. */
    if (!s_sd_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo_cfg = {.ldo_chan_id = SD_LDO_CHAN};
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_sd_pwr) != ESP_OK) {
            ESP_LOGW(TAG, "TF card power (LDO %d) unavailable; card slot not used", SD_LDO_CHAN);
            s_sd_pwr = NULL;
            return false;
        }
    }
    host.pwr_ctrl_handle = s_sd_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = s_card_modes[s_card_step].width;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.d1 = PIN_SD_D1;
    slot.d2 = PIN_SD_D2;
    slot.d3 = PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (sdmmc_host_init() != ESP_OK) {
        return false;
    }
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot) != ESP_OK ||
        sdmmc_card_init(&host, &s_card) != ESP_OK) {
        sdmmc_host_deinit();
        return false;
    }
    return true;
}

/* Called at start-up and at every reboot of the emulated PC: pick up a card that was
 * inserted, drop one that was removed. DOS only looks for drives when it boots. */
static void card_refresh(void)
{
    if (s_card_ok && sdmmc_get_status(&s_card) != ESP_OK) {
        ESP_LOGW(TAG, "TF card removed");
        sdmmc_host_deinit();
        s_card_ok = false;
    }
    if (!s_card_ok && card_open()) {
        s_card_ok = true;
        s_card_dev.readonly = 0;
        s_card_dev.sectors = (uint32_t)s_card.csd.capacity;
        pc_attach(DRIVE_HD1, &s_card_dev);
        ESP_LOGW(TAG, "TF card: %u MB, drive D:",
                 (unsigned)((uint64_t)s_card.csd.capacity * s_card.csd.sector_size >> 20));
    } else if (!s_card_ok) {
        pc_attach(DRIVE_HD1, NULL); /* removed, or a restart after an error failed */
    }
}

#endif /* raw card */

/* --- image files on the TF card ------------------------------------------------- */

#if CONFIG_P4DOS_SD_CARD && CONFIG_P4DOS_SD_IMAGES

typedef struct {
    int fd;
    int dirty;
} filedisk_t;

static int file_read(blkdev_t *d, uint32_t lba, uint32_t n, uint8_t *buf)
{
    filedisk_t *f = d->ctx;
    if (lseek(f->fd, (off_t)lba * 512, SEEK_SET) < 0) {
        return -1;
    }
    return read(f->fd, buf, (size_t)n * 512) == (ssize_t)(n * 512) ? 0 : -1;
}

static int file_write(blkdev_t *d, uint32_t lba, uint32_t n, const uint8_t *buf)
{
    filedisk_t *f = d->ctx;
    if (lseek(f->fd, (off_t)lba * 512, SEEK_SET) < 0) {
        return -1;
    }
    f->dirty = 1;
    return write(f->fd, buf, (size_t)n * 512) == (ssize_t)(n * 512) ? 0 : -1;
}

static void file_flush(blkdev_t *d)
{
    filedisk_t *f = d->ctx;
    if (f->dirty) {
        f->dirty = 0;
        fsync(f->fd);
    }
}

static void attach_image(int drive, const char *path)
{
    static blkdev_t devs[DRIVE_COUNT];
    static filedisk_t files[DRIVE_COUNT];
    struct stat st;
    int readonly = 0;
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fd = open(path, O_RDONLY);
        readonly = 1;
    }
    if (fd < 0) {
        return;
    }
    if (fstat(fd, &st) || st.st_size < 512) {
        close(fd);
        return;
    }
    files[drive].fd = fd;
    files[drive].dirty = 0;
    devs[drive] = (blkdev_t){
        .read = file_read,
        .write = file_write,
        .flush = file_flush,
        .sectors = (uint32_t)(st.st_size / 512),
        .readonly = readonly,
        .ctx = &files[drive],
    };
    pc_attach(drive, &devs[drive]);
    ESP_LOGI(TAG, "%s attached (%u KB%s)", path, (unsigned)(st.st_size / 1024),
             readonly ? ", read-only" : "");
}

static void sd_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

    /* The slot is powered from the P4's internal LDO channel 4. */
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {.ldo_chan_id = SD_LDO_CHAN};
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) != ESP_OK) {
        ESP_LOGW(TAG, "TF card power (LDO %d) unavailable; card slot not used", SD_LDO_CHAN);
        return;
    }
    host.pwr_ctrl_handle = pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.d1 = PIN_SD_D1;
    slot.d2 = PIN_SD_D2;
    slot.d3 = PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no TF card mounted (%s)", esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(pwr);
        return;
    }
    attach_image(DRIVE_FD0, "/sdcard/P4DOS/A.IMG");
    attach_image(DRIVE_FD1, "/sdcard/P4DOS/B.IMG");
    attach_image(DRIVE_HD1, "/sdcard/P4DOS/HD.IMG");
}

#endif /* image files */

void storage_init(void)
{
    flash_disk_init();
#if CONFIG_P4DOS_SD_CARD && CONFIG_P4DOS_SD_IMAGES
    sd_init();
#endif
    hal_disks_refresh();
}

void hal_disks_refresh(void)
{
#if CONFIG_P4DOS_SD_CARD && CONFIG_P4DOS_SD_RAW
    card_refresh();
#endif
}

void storage_tick(void)
{
    disk_flush();
}
