set(core_dir "${CMAKE_CURRENT_LIST_DIR}/../../../core")

idf_component_register(
    SRCS "main.c" "display.c" "usbhid.c" "storage.c" "console.c" "rtc.c" "sound.c"
         "${core_dir}/cpu.c" "${core_dir}/pc.c" "${core_dir}/bios.c" "${core_dir}/biosvid.c"
         "${core_dir}/disk.c" "${core_dir}/hostdisk.c" "${core_dir}/kbd.c" "${core_dir}/uart.c" "${core_dir}/ems.c" "${core_dir}/xms.c" "${core_dir}/sb.c" "${core_dir}/audio.c"
         "${core_dir}/video.c" "${core_dir}/render.c" "${core_dir}/term.c"
         "${core_dir}/rom.c" "${core_dir}/vgapal.c"
    INCLUDE_DIRS "." "${core_dir}"
    REQUIRES esp_lcd esp_lcd_jd9365 usb usb_host_hid fatfs sdmmc esp_driver_sdmmc
             esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_codec_dev nvs_flash
             esp_driver_usb_serial_jtag esp_partition
             spi_flash esp_timer esp_psram esp_mm esp_hw_support
)

# The interpreter loads and stores guest words in one access at any address (the ESP32-P4
# does misaligned accesses in hardware; GCC only emits them as single instructions under a
# tuning that does not call unaligned access slow), and its run loop is written to be laid
# out in source order (see core/cpu.c).
set(cpu_options "-mno-strict-align;-mtune=thead-c906;-fno-reorder-blocks")
set_source_files_properties("${core_dir}/cpu.c" PROPERTIES COMPILE_OPTIONS "${cpu_options}")

# The renderer's 64 KB working buffer goes to PSRAM: internal RAM is kept for ESP-IDF.
set_source_files_properties("${core_dir}/render.c" PROPERTIES COMPILE_OPTIONS "-include;esp_attr.h"
                            COMPILE_DEFINITIONS "HAL_EXT_BSS=EXT_RAM_BSS_ATTR")

# Flash the FreeDOS hard disk image into the "dosdisk" partition together with the app.
# Build it first with `make image` in the repository root.
set(disk_image "${CMAKE_CURRENT_LIST_DIR}/../../../build/p4dos-hd.img")
if(EXISTS "${disk_image}")
    esptool_py_flash_to_partition(flash "dosdisk" "${disk_image}")
else()
    message(WARNING "build/p4dos-hd.img not found: run `make image`; C: will not be flashed")
endif()
