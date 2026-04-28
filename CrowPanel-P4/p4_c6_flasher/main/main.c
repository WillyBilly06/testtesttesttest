/*
 * P4-as-C6-flasher — transparent USB-CDC <-> UART1 bridge.
 *
 * Wiring (CrowPanel-P4 + on-board C6):
 *   P4 GPIO 47 (TX) -> C6 UART0 RX (GPIO 17)
 *   P4 GPIO 48 (RX) <- C6 UART0 TX (GPIO 16)
 *   P4 GPIO 33      -> C6 BOOT (GPIO 9)     active LOW
 *   P4 GPIO 32      -> C6 EN   (CHIP_PU)    active LOW
 *
 *   (matches the canonical mapping used by
 *    espressif__esp_wifi_remote/examples/mqtt/sdkconfig.ci.p4 :
 *      CONFIG_WIFI_RMT_OVER_EPPP_UART_TX_PIN=47
 *      CONFIG_WIFI_RMT_OVER_EPPP_UART_RX_PIN=48 )
 *
 * Auto-reset (matches CP2102/CH340, which is what esptool drives):
 *   Host asserts DTR -> BOOT LOW  (download strap)
 *   Host asserts RTS -> EN   LOW  (reset)
 *   Neither asserted -> both HIGH (normal run)
 *
 * After flashing this app to the P4:
 *   1. Plug the P4's USB-C into the PC. A new COM port appears (the P4's
 *      native USB-CDC, NOT the USB-Serial-JTAG one).
 *   2. Use that COM port with esptool, e.g.
 *        python -m esptool --chip esp32c6 -p COMx -b 460800 \
 *            write_flash 0x0 crowpanel_c6_firmware.bin
 *
 * To return to normal P4 behaviour: reflash the factory firmware.
 */

#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "tinyusb.h"
#include "tusb_cdc_acm.h"

static const char *TAG = "p4_c6_flasher";

/* ── Hardware pins (see Wiring above) ──────────────────────────── */
#define C6_UART_NUM     UART_NUM_1
#define C6_UART_RX_PIN  48   /* P4 RX <- C6 TX */
#define C6_UART_TX_PIN  47   /* P4 TX -> C6 RX */
#define C6_BOOT_PIN     33
#define C6_EN_PIN       32

#define DEFAULT_BAUD    115200
#define UART_BUF_SIZE   4096

#define USB_BRIDGE_BUF  512

/* The CDC interface index we're using. */
#define CDC_ITF         TINYUSB_CDC_ACM_0

/* ── State ─────────────────────────────────────────────────────── */
static volatile uint32_t s_current_baud   = DEFAULT_BAUD;
static volatile uint32_t s_cdc_rx_total   = 0;  /* host -> bridge bytes  */
static volatile uint32_t s_uart_tx_total  = 0;  /* bridge -> C6 bytes    */
static volatile uint32_t s_uart_rx_total  = 0;  /* C6 -> bridge bytes    */
static volatile uint32_t s_cdc_tx_total   = 0;  /* bridge -> host bytes  */
static volatile uint32_t s_cdc_tx_dropped = 0;
static volatile uint32_t s_line_state_evt = 0;
static volatile uint32_t s_line_coding_evt= 0;

/* ── Helpers ───────────────────────────────────────────────────── */

static inline void apply_line_state(bool dtr, bool rts)
{
    /* Host-asserted line (=1) pulls the target pin LOW, matching CP2102. */
    gpio_set_level(C6_BOOT_PIN, dtr ? 0 : 1);
    gpio_set_level(C6_EN_PIN,   rts ? 0 : 1);
    s_line_state_evt++;
    ESP_EARLY_LOGI(TAG, "line state: DTR=%d RTS=%d -> BOOT=%d EN=%d",
                   dtr, rts, dtr ? 0 : 1, rts ? 0 : 1);
}

static void apply_baud(uint32_t baud)
{
    s_line_coding_evt++;
    if (baud == 0 || baud == s_current_baud) return;
    s_current_baud = baud;

    esp_err_t err = uart_set_baudrate(C6_UART_NUM, baud);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_baudrate(%lu): %s",
                 (unsigned long)baud, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "baud -> %lu", (unsigned long)baud);
    }
}

/* ── CDC callbacks ─────────────────────────────────────────────── */

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    /* Drain everything the host sent and forward to the C6's UART. */
    uint8_t buf[USB_BRIDGE_BUF];
    size_t  rx_size = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx_size) == ESP_OK
           && rx_size > 0) {
        s_cdc_rx_total += rx_size;
        int wr = uart_write_bytes(C6_UART_NUM, (const char *)buf, rx_size);
        if (wr > 0) s_uart_tx_total += (uint32_t)wr;
        rx_size = 0;
    }
    (void)event;
}

static void cdc_line_state_changed_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    apply_line_state(dtr, rts);
}

static void cdc_line_coding_changed_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    uint32_t baud = event->line_coding_changed_data.p_line_coding->bit_rate;
    apply_baud(baud);
}

/* ── UART -> USB pump task ─────────────────────────────────────── */
/* CDC RX is callback-driven (USB direction is host -> C6).
 * UART RX direction (C6 -> USB) needs a polling task because the IDF
 * UART driver delivers via blocking reads / events, and we want a tight
 * latency loop without any rate limits.
 */
static void uart_to_usb_task(void *arg)
{
    (void)arg;
    uint8_t buf[USB_BRIDGE_BUF];

    while (1) {
        int n = uart_read_bytes(C6_UART_NUM, buf, sizeof(buf),
                                pdMS_TO_TICKS(5));
        if (n <= 0) continue;
        s_uart_rx_total += (uint32_t)n;

        /* Pump unconditionally -- never gate on DTR/RTS or mount state.
         * esptool keeps DTR=0 for most of its session, so any host-state
         * check here would silently drop the C6's sync replies. The
         * harmless "tusb_cdc_acm: Flush failed" warnings emitted before
         * a host is connected are silenced via esp_log_level_set() in
         * app_main(). */

        /* Forward every single byte. esptool's SLIP framing has zero
         * tolerance for dropped bytes during stub upload, so we keep
         * retrying queue+flush until the whole batch is on the wire.
         * To avoid hanging forever if the host disappears mid-flash,
         * we cap the wait at ~400ms before dropping. */
        size_t off = 0;
        int stalls = 0;
        while (off < (size_t)n) {
            size_t chunk  = (size_t)n - off;
            size_t wrote  = tinyusb_cdcacm_write_queue(CDC_ITF,
                                                       buf + off, chunk);
            off += wrote;
            if (wrote < chunk) {
                /* TX queue full -- push it out then yield.            */
                tinyusb_cdcacm_write_flush(CDC_ITF, pdMS_TO_TICKS(20));
                if (++stalls > 20) {
                    s_cdc_tx_dropped += (n - off);
                    ESP_LOGW(TAG, "uart->usb backpressure, dropping %u bytes",
                             (unsigned)(n - off));
                    break;
                }
            }
        }
        /* Trigger immediate IN transfer so the host gets ACKs/echoes
         * with minimum latency. esptool times out fast (~1s) on the
         * stub-upload handshake. */
        tinyusb_cdcacm_write_flush(CDC_ITF, 0);
        s_cdc_tx_total += (uint32_t)off;
    }
}

/* ── Init helpers ──────────────────────────────────────────────── */

static void init_gpio(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << C6_BOOT_PIN) | (1ULL << C6_EN_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(C6_BOOT_PIN, 1);   /* idle high  -> BOOT not asserted */
    gpio_set_level(C6_EN_PIN,   1);   /* idle high  -> not in reset      */
}

static void init_uart(void)
{
    const uart_config_t cfg = {
        .baud_rate = DEFAULT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(C6_UART_NUM,
                                        UART_BUF_SIZE,
                                        UART_BUF_SIZE,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(C6_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(C6_UART_NUM,
                                 C6_UART_TX_PIN,
                                 C6_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

static void init_usb_cdc(void)
{
    /* Zero-init keeps every descriptor pointer NULL, which tells the
     * driver to use the Kconfig-generated defaults (VID/PID/strings). */
    const tinyusb_config_t tusb_cfg = { 0 };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev                      = TINYUSB_USBDEV_0,
        .cdc_port                     = CDC_ITF,
        .callback_rx                  = cdc_rx_cb,
        .callback_rx_wanted_char      = NULL,
        .callback_line_state_changed  = cdc_line_state_changed_cb,
        .callback_line_coding_changed = cdc_line_coding_changed_cb,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
}

/* ── Entry ─────────────────────────────────────────────────────── */

void app_main(void)
{
    /* Big banner so it's obvious the bridge firmware is running when
     * you `idf.py monitor` over the USB-Serial-JTAG COM port. */
    ESP_LOGW(TAG, "================================================");
    ESP_LOGW(TAG, "  P4-as-C6-flasher v1 booting");
    ESP_LOGW(TAG, "  UART%d  RX=GPIO%d  TX=GPIO%d",
             C6_UART_NUM, C6_UART_RX_PIN, C6_UART_TX_PIN);
    ESP_LOGW(TAG, "  BOOT=GPIO%d  EN=GPIO%d",
             C6_BOOT_PIN, C6_EN_PIN);
    ESP_LOGW(TAG, "================================================");

    /* esp_tinyusb prints a benign "Flush failed" warning every time we
     * try to flush while the host isn't reading. It's pure noise and
     * clutters the bridge log during esptool reset/reconnect cycles. */
    esp_log_level_set("tusb_cdc_acm", ESP_LOG_ERROR);

    ESP_LOGI(TAG, "init_gpio ...");    init_gpio();
    ESP_LOGI(TAG, "init_uart ...");    init_uart();
    ESP_LOGI(TAG, "init_usb_cdc ..."); init_usb_cdc();
    ESP_LOGI(TAG, "init_usb_cdc -> OK (TinyUSB CDC on USB-OTG-HS)");

    /* Pinned to core 1 to keep USB stack on core 0 untouched. */
    xTaskCreatePinnedToCore(uart_to_usb_task, "uart2usb",
                            4096, NULL, 12, NULL, 1);

    ESP_LOGW(TAG, "READY  -- a new COM port should now appear on the");
    ESP_LOGW(TAG, "         OTHER USB-C connector. Use it with esptool.");

    /* Heartbeat with traffic counters so we can spot which direction
     * is silent during an esptool failure. */
    int beat = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG,
                 "alive(%d) usb=%d dtr=%d ls=%lu lc=%lu  H->C: cdc_rx=%lu "
                 "utx=%lu  C->H: urx=%lu cdc_tx=%lu drop=%lu",
                 ++beat,
                 tud_mounted(),
                 tud_cdc_n_connected(CDC_ITF),
                 (unsigned long)s_line_state_evt,
                 (unsigned long)s_line_coding_evt,
                 (unsigned long)s_cdc_rx_total,
                 (unsigned long)s_uart_tx_total,
                 (unsigned long)s_uart_rx_total,
                 (unsigned long)s_cdc_tx_total,
                 (unsigned long)s_cdc_tx_dropped);
    }
}
