/*
 * USB host on the high-speed OTG port (middle USB-C): keyboards and mice, directly, behind
 * a hub, or through a wireless receiver. Keyboard reports feed the emulated XT keyboard;
 * mouse movement feeds the Microsoft serial mouse on COM1.
 *
 * A Logitech Unifying receiver is one device with three HID interfaces: a boot keyboard,
 * a boot mouse and Logitech's own HID++ channel. The first two are switched to the boot
 * protocol like any keyboard and mouse; should the receiver keep its mouse interface in
 * report protocol, its report layout (ID 2, 12-bit X/Y) is understood as well. The HID++
 * interface is opened but its reports are ignored.
 */
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pc.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "usbhid";

#define MAX_KEYBOARDS 4

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} dev_evt_t;

#define MAX_IFACES 8

/* What is known about each open HID interface. */
typedef struct {
    hid_host_device_handle_t handle;
    bool boot_protocol; /* SET_PROTOCOL(boot) was accepted */
    bool seen_report;
} iface_t;

static QueueHandle_t s_dev_q;
static hid_host_device_handle_t s_keyboards[MAX_KEYBOARDS];
static iface_t s_ifaces[MAX_IFACES];

static iface_t *iface_find(hid_host_device_handle_t h)
{
    for (int i = 0; i < MAX_IFACES; i++) {
        if (s_ifaces[i].handle == h) {
            return &s_ifaces[i];
        }
    }
    return NULL;
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    xTaskNotify((TaskHandle_t)arg, err == ESP_OK, eSetValueWithOverwrite);
    if (err != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* HID host background task: one input report, or the interface went away. */
static void iface_cb(hid_host_device_handle_t h, const hid_host_interface_event_t ev, void *arg)
{
    uint8_t data[64];
    size_t len = 0;
    hid_host_dev_params_t p;
    (void)arg;
    if (hid_host_device_get_params(h, &p) != ESP_OK) {
        return;
    }
    const bool boot = p.sub_class == HID_SUBCLASS_BOOT_INTERFACE;

    switch (ev) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) != ESP_OK) {
            return;
        }
        iface_t *it = iface_find(h);
        if (it && !it->seen_report) {
            it->seen_report = true;
            ESP_LOGI(TAG, "first report on interface proto %d: %u bytes, %02x %02x %02x %02x", p.proto,
                     (unsigned)len, data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
                     len > 3 ? data[3] : 0);
        }
        if (boot && p.proto == HID_PROTOCOL_KEYBOARD && len >= 8) {
            kbd_hid_report(data[0], &data[2], 6); /* [modifiers][reserved][6 keys] */
            p4dos_wake();
        } else if (boot && p.proto == HID_PROTOCOL_MOUSE && it && !it->boot_protocol && len >= 6 &&
                   data[0] == 0x02) {
            /* Logitech receiver in report protocol: [02][buttons:16][X:12][Y:12][wheel].. */
            int dx = data[3] | ((data[4] & 0x0F) << 8);
            int dy = (data[4] >> 4) | (data[5] << 4);
            mouse_event(dx >= 0x800 ? dx - 0x1000 : dx, dy >= 0x800 ? dy - 0x1000 : dy, data[1] & 3);
            p4dos_wake();
        } else if (boot && p.proto == HID_PROTOCOL_MOUSE && len >= 3) {
            mouse_event((int8_t)data[1], (int8_t)data[2], data[0] & 3); /* [buttons][dx][dy] */
            p4dos_wake();
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        if (boot && p.proto == HID_PROTOCOL_KEYBOARD) {
            static const uint8_t none[6];
            kbd_hid_report(0, none, 6); /* release whatever was held */
            for (int i = 0; i < MAX_KEYBOARDS; i++) {
                if (s_keyboards[i] == h) {
                    s_keyboards[i] = NULL;
                }
            }
        }
        iface_t *gone = iface_find(h);
        if (gone) {
            gone->handle = NULL;
        }
        ESP_LOGI(TAG, "HID interface disconnected (proto %d)", p.proto);
        hid_host_device_close(h);
        break;
    default: break;
    }
}

/* The product name the device reports, e.g. "Logitech USB Receiver". */
static void log_device(hid_host_device_handle_t h, const hid_host_dev_params_t *p)
{
    static hid_host_dev_info_t di; /* wide strings: too big for the stack */
    char name[48] = "";
    if (hid_host_get_device_info(h, &di) == ESP_OK) {
        size_t o = 0;
        for (size_t i = 0; i < HID_STR_DESC_MAX_LENGTH && di.iProduct[i] && o + 1 < sizeof(name); i++) {
            name[o++] = (di.iProduct[i] >= 0x20 && di.iProduct[i] < 0x7F) ? (char)di.iProduct[i] : '?';
        }
        name[o] = 0;
        ESP_LOGI(TAG, "%04X:%04X \"%s\" interface %d: sub_class %d, proto %d", di.VID, di.PID, name,
                 p->iface_num, p->sub_class, p->proto);
    } else {
        ESP_LOGI(TAG, "HID interface %d: sub_class %d, proto %d", p->iface_num, p->sub_class,
                 p->proto);
    }
}

/* Opening a device issues blocking control transfers, so it runs in our own task. */
static void device_connected(hid_host_device_handle_t h)
{
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(h, &p) != ESP_OK) {
        return;
    }

    const hid_host_device_config_t cfg = {.callback = iface_cb, .callback_arg = NULL};
    esp_err_t err = hid_host_device_open(h, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open: %s", esp_err_to_name(err));
        return;
    }
    log_device(h, &p);

    iface_t *it = iface_find(NULL); /* a free slot */
    if (it) {
        it->handle = h;
        it->boot_protocol = false;
        it->seen_report = false;
    }
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        err = hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_protocol(boot) on interface %d: %s", p.iface_num, esp_err_to_name(err));
        } else if (it) {
            it->boot_protocol = true;
        }
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            hid_class_request_set_idle(h, 0, 0);
            for (int i = 0; i < MAX_KEYBOARDS; i++) {
                if (!s_keyboards[i]) {
                    s_keyboards[i] = h;
                    break;
                }
            }
        }
    }
    err = hid_host_device_start(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_start: %s", esp_err_to_name(err));
    }
}

static void device_cb(hid_host_device_handle_t h, const hid_host_driver_event_t ev, void *arg)
{
    const dev_evt_t e = {.handle = h, .event = ev};
    (void)arg;
    xQueueSend(s_dev_q, &e, 0);
}

static void events_task(void *arg)
{
    uint8_t shown_leds = 0xFF;
    int shown_devices = -1;
    (void)arg;
    for (;;) {
        dev_evt_t e;
        /* Say when the number of devices on the bus changes: tells "nothing plugged in"
         * apart from "plugged in but not usable" when a keyboard does not respond. */
        uint8_t addrs[8];
        int ndev = 0;
        if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &ndev) == ESP_OK &&
            ndev != shown_devices && xTaskGetTickCount() > pdMS_TO_TICKS(4000)) {
            shown_devices = ndev;
            ESP_LOGI(TAG, "USB devices on the bus: %d", ndev);
        }
        if (xQueueReceive(s_dev_q, &e, pdMS_TO_TICKS(100))) {
            if (e.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
                device_connected(e.handle);
                shown_leds = 0xFF;
            }
        }
        /* Keyboard LEDs follow the BIOS lock state (bit0 Scroll, bit1 Num, bit2 Caps). */
        uint8_t leds = kbd_leds();
        if (leds != shown_leds) {
            uint8_t report = (uint8_t)(((leds >> 1) & 1) | ((leds >> 1) & 2) | ((leds & 1) << 2));
            shown_leds = leds;
            for (int i = 0; i < MAX_KEYBOARDS; i++) {
                if (s_keyboards[i]) {
                    hid_class_request_set_report(s_keyboards[i], HID_REPORT_TYPE_OUTPUT, 0,
                                                 &report, 1);
                }
            }
        }
    }
}

esp_err_t usbhid_start(void)
{
    uint32_t ok = 0;

    s_dev_q = xQueueCreate(8, sizeof(dev_evt_t));
    if (!s_dev_q) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 2,
                                NULL, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!xTaskNotifyWait(0, 0, &ok, pdMS_TO_TICKS(3000)) || !ok) {
        return ESP_FAIL;
    }

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = device_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_install(&hid_cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreatePinnedToCore(events_task, "usb_evt", 4096, NULL, 4, NULL, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    /* The serial mouse on COM1 is always there for the DOS mouse driver to find, whether
     * or not a USB mouse is plugged in yet: a receiver or mouse added later just works. */
    mouse_present = 1;
    ESP_LOGI(TAG, "USB host started; waiting for a keyboard or receiver");
    return ESP_OK;
}
