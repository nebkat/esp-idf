/* UART Event Callbacks Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "uart_callbacks";

/**
 * This example shows how to use uart_register_event_callbacks() to drive a UART port from a single
 * application event loop, without a task dedicated to blocking on the driver's event queue.
 *
 * The driver is installed with `uart_queue` set to NULL, so no event queue is created. The callbacks run in
 * ISR context and do nothing but post a small fixed-size record onto the application's own loop queue.
 * app_main() is the only task that touches the port.
 *
 * - Port: UART0
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: off, callbacks instead
 * - Pin assignment: TxD (default), RxD (default)
 */

#define EX_UART_NUM         UART_NUM_0
#define PATTERN_CHR_NUM     (3)     /*!< Number of consecutive identical characters that define the pattern */

#define BUF_SIZE            (1024)
#define LOOP_QUEUE_LEN      (20)

// What the callbacks hand to the application loop. Small, fixed size, no allocation and no copy of the
// received bytes: the data stays in the driver's RX ring buffer until the loop reads it.
typedef enum {
    APP_EVENT_RX_DATA,
    APP_EVENT_RX_BREAK,
    APP_EVENT_PATTERN,
    APP_EVENT_RX_ERROR,
    APP_EVENT_TX_IDLE,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    size_t size;                /*!< Bytes available, for APP_EVENT_RX_DATA */
    uart_event_type_t error;    /*!< Which error, for APP_EVENT_RX_ERROR */
} app_event_t;

// The callbacks run in ISR context. Only ...FromISR() APIs may be called from them, and they must not
// block. With CONFIG_UART_ISR_IN_IRAM enabled they must also be in IRAM, hence IRAM_ATTR, and the loop
// queue handle passed as user context has to live in internal RAM.

static IRAM_ATTR bool on_rx_data(uart_port_t uart_num, const uart_rx_data_event_data_t *edata, void *user_ctx)
{
    QueueHandle_t loop_queue = (QueueHandle_t)user_ctx;
    BaseType_t task_woken = pdFALSE;
    app_event_t evt = {
        .type = APP_EVENT_RX_DATA,
        .size = edata->size,
    };
    (void)uart_num;
    xQueueSendFromISR(loop_queue, &evt, &task_woken);
    return task_woken == pdTRUE;
}

static IRAM_ATTR bool on_rx_break(uart_port_t uart_num, const uart_rx_break_event_data_t *edata, void *user_ctx)
{
    QueueHandle_t loop_queue = (QueueHandle_t)user_ctx;
    BaseType_t task_woken = pdFALSE;
    app_event_t evt = { .type = APP_EVENT_RX_BREAK };
    (void)uart_num;
    (void)edata;
    xQueueSendFromISR(loop_queue, &evt, &task_woken);
    return task_woken == pdTRUE;
}

static IRAM_ATTR bool on_pattern(uart_port_t uart_num, const uart_pattern_event_data_t *edata, void *user_ctx)
{
    QueueHandle_t loop_queue = (QueueHandle_t)user_ctx;
    BaseType_t task_woken = pdFALSE;
    app_event_t evt = { .type = APP_EVENT_PATTERN };
    (void)uart_num;
    (void)edata;
    xQueueSendFromISR(loop_queue, &evt, &task_woken);
    return task_woken == pdTRUE;
}

static IRAM_ATTR bool on_rx_error(uart_port_t uart_num, const uart_rx_error_event_data_t *edata, void *user_ctx)
{
    QueueHandle_t loop_queue = (QueueHandle_t)user_ctx;
    BaseType_t task_woken = pdFALSE;
    app_event_t evt = {
        .type = APP_EVENT_RX_ERROR,
        .error = edata->type,
    };
    (void)uart_num;
    xQueueSendFromISR(loop_queue, &evt, &task_woken);
    return task_woken == pdTRUE;
}

static IRAM_ATTR bool on_tx_done(uart_port_t uart_num, const uart_tx_done_event_data_t *edata, void *user_ctx)
{
    QueueHandle_t loop_queue = (QueueHandle_t)user_ctx;
    BaseType_t task_woken = pdFALSE;
    app_event_t evt = { .type = APP_EVENT_TX_IDLE };
    (void)uart_num;
    if (!edata->flags.tx_idle) {
        // TX ring buffer space was returned. This example has no TX buffer and nothing to throttle, so
        // only the "transmitter idle" case is reported to the loop.
        return false;
    }
    xQueueSendFromISR(loop_queue, &evt, &task_woken);
    return task_woken == pdTRUE;
}

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // The application's own event loop queue. In a real application this would multiplex several sources.
    QueueHandle_t loop_queue = xQueueCreate(LOOP_QUEUE_LEN, sizeof(app_event_t));
    assert(loop_queue);

    // Install with uart_queue == NULL: no driver event queue, and therefore no task that has to block on it
    ESP_ERROR_CHECK(uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(EX_UART_NUM, &uart_config));

    uart_event_callbacks_t cbs = {
        .on_rx_data = on_rx_data,
        .on_rx_break = on_rx_break,
        .on_pattern = on_pattern,
        .on_rx_error = on_rx_error,
        .on_tx_done = on_tx_done,
    };
    ESP_ERROR_CHECK(uart_register_event_callbacks(EX_UART_NUM, &cbs, loop_queue));

    // Detect "+++" in the incoming stream
    ESP_ERROR_CHECK(uart_enable_pattern_det_baud_intr(EX_UART_NUM, '+', PATTERN_CHR_NUM, 9, 0, 0));
    ESP_ERROR_CHECK(uart_pattern_queue_reset(EX_UART_NUM, LOOP_QUEUE_LEN));

    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);
    assert(data);

    ESP_LOGI(TAG, "waiting for UART events on a single application loop, no UART task");

    app_event_t evt;
    while (1) {
        if (xQueueReceive(loop_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case APP_EVENT_RX_DATA:
            // The callback only told us how much is waiting. Reading happens here, in task context.
            ESP_LOGI(TAG, "[UART DATA]: %d", (int)evt.size);
            uart_read_bytes(EX_UART_NUM, data, evt.size, portMAX_DELAY);
            uart_write_bytes(EX_UART_NUM, (const char *)data, evt.size);
            break;
        case APP_EVENT_RX_BREAK:
            ESP_LOGI(TAG, "[UART BREAK]");
            break;
        case APP_EVENT_PATTERN: {
            // The position also comes out of the driver from task context, not from the callback
            int pos = uart_pattern_pop_pos(EX_UART_NUM);
            ESP_LOGI(TAG, "[UART PATTERN DETECTED] pos: %d", pos);
            if (pos == -1) {
                // The pattern position queue overflowed, the oldest positions were lost
                uart_flush_input(EX_UART_NUM);
            } else {
                uart_read_bytes(EX_UART_NUM, data, pos, 100 / portTICK_PERIOD_MS);
                uint8_t pat[PATTERN_CHR_NUM + 1] = {};
                uart_read_bytes(EX_UART_NUM, pat, PATTERN_CHR_NUM, 100 / portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "read data: %s, read pattern: %s", data, pat);
            }
            break;
        }
        case APP_EVENT_RX_ERROR:
            switch (evt.error) {
            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "[UART FIFO OVERFLOW]");
                // Consider adding flow control. The ISR has already reset the RX FIFO.
                uart_flush_input(EX_UART_NUM);
                break;
            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "[UART RING BUFFER FULL]");
                // Consider increasing the RX buffer size passed to uart_driver_install()
                uart_flush_input(EX_UART_NUM);
                break;
            case UART_FRAME_ERR:
                ESP_LOGI(TAG, "[UART FRAME ERROR]");
                break;
            case UART_PARITY_ERR:
                ESP_LOGI(TAG, "[UART PARITY ERROR]");
                break;
            default:
                break;
            }
            break;
        case APP_EVENT_TX_IDLE:
            ESP_LOGD(TAG, "[UART TX IDLE]");
            break;
        }
    }
}
