/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "sdkconfig.h"

#define TEST_CB_UART_NUM   UART_NUM_1
#define TEST_CB_BAUD_RATE  115200
#define TEST_CB_RX_BUF     1024
#define TEST_CB_TX_BUF     1024

// The callbacks run in ISR context, so everything they touch has to live in internal RAM. A file scope
// static lands in .bss, which is internal, and the semaphores come from the internal heap.
typedef struct {
    SemaphoreHandle_t rx_data_sem;
    SemaphoreHandle_t pattern_sem;
    SemaphoreHandle_t rx_error_sem;
    SemaphoreHandle_t tx_idle_sem;
    volatile int rx_data_calls;
    volatile int rx_bytes;
    volatile bool rx_timeout_seen;
    volatile int pattern_calls;
    volatile int rx_error_calls;
    volatile uart_event_type_t last_rx_error;
    volatile int tx_space_calls;
    volatile int tx_idle_calls;
    volatile int wrong_port_calls;
    volatile int wrong_ctx_calls;
} test_cb_state_t;

static test_cb_state_t s_state;

static void test_cb_state_init(void)
{
    memset((void *)&s_state, 0, sizeof(s_state));
    s_state.rx_data_sem = xSemaphoreCreateCounting(64, 0);
    s_state.pattern_sem = xSemaphoreCreateCounting(64, 0);
    s_state.rx_error_sem = xSemaphoreCreateCounting(64, 0);
    s_state.tx_idle_sem = xSemaphoreCreateCounting(64, 0);
    TEST_ASSERT_NOT_NULL(s_state.rx_data_sem);
    TEST_ASSERT_NOT_NULL(s_state.pattern_sem);
    TEST_ASSERT_NOT_NULL(s_state.rx_error_sem);
    TEST_ASSERT_NOT_NULL(s_state.tx_idle_sem);
}

static void test_cb_state_deinit(void)
{
    vSemaphoreDelete(s_state.rx_data_sem);
    vSemaphoreDelete(s_state.pattern_sem);
    vSemaphoreDelete(s_state.rx_error_sem);
    vSemaphoreDelete(s_state.tx_idle_sem);
    memset((void *)&s_state, 0, sizeof(s_state));
}

static IRAM_ATTR bool test_on_rx_data(uart_port_t uart_num, const uart_rx_data_event_data_t *edata, void *user_ctx)
{
    test_cb_state_t *st = (test_cb_state_t *)user_ctx;
    BaseType_t task_woken = pdFALSE;
    if (uart_num != TEST_CB_UART_NUM) {
        st->wrong_port_calls++;
    }
    if (st != &s_state) {
        st->wrong_ctx_calls++;
    }
    st->rx_data_calls++;
    st->rx_bytes += edata->size;
    if (edata->timeout_flag) {
        st->rx_timeout_seen = true;
    }
    xSemaphoreGiveFromISR(st->rx_data_sem, &task_woken);
    return task_woken == pdTRUE;
}

static IRAM_ATTR bool test_on_pattern(uart_port_t uart_num, const uart_pattern_event_data_t *edata, void *user_ctx)
{
    test_cb_state_t *st = (test_cb_state_t *)user_ctx;
    BaseType_t task_woken = pdFALSE;
    (void)uart_num;
    (void)edata;
    st->pattern_calls++;
    xSemaphoreGiveFromISR(st->pattern_sem, &task_woken);
    return task_woken == pdTRUE;
}

static IRAM_ATTR bool test_on_rx_error(uart_port_t uart_num, const uart_rx_error_event_data_t *edata, void *user_ctx)
{
    test_cb_state_t *st = (test_cb_state_t *)user_ctx;
    BaseType_t task_woken = pdFALSE;
    (void)uart_num;
    st->rx_error_calls++;
    st->last_rx_error = edata->type;
    xSemaphoreGiveFromISR(st->rx_error_sem, &task_woken);
    return task_woken == pdTRUE;
}

static IRAM_ATTR bool test_on_tx_done(uart_port_t uart_num, const uart_tx_done_event_data_t *edata, void *user_ctx)
{
    test_cb_state_t *st = (test_cb_state_t *)user_ctx;
    BaseType_t task_woken = pdFALSE;
    (void)uart_num;
    if (edata->flags.tx_idle) {
        st->tx_idle_calls++;
        xSemaphoreGiveFromISR(st->tx_idle_sem, &task_woken);
    } else {
        st->tx_space_calls++;
    }
    return task_woken == pdTRUE;
}

static void test_cb_uart_setup(int rx_buf_size, int tx_buf_size, int queue_size, QueueHandle_t *out_queue)
{
    uart_config_t uart_config = {
        .baud_rate = TEST_CB_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    TEST_ESP_OK(uart_driver_install(TEST_CB_UART_NUM, rx_buf_size, tx_buf_size, queue_size, out_queue, 0));
    TEST_ESP_OK(uart_param_config(TEST_CB_UART_NUM, &uart_config));
    // Loop TX back to RX internally, so the test needs no wiring and no pins.
    TEST_ESP_OK(uart_set_loop_back(TEST_CB_UART_NUM, true));
    TEST_ESP_OK(uart_flush_input(TEST_CB_UART_NUM));
}

static void test_cb_uart_teardown(void)
{
    TEST_ESP_OK(uart_set_loop_back(TEST_CB_UART_NUM, false));
    TEST_ESP_OK(uart_driver_delete(TEST_CB_UART_NUM));
}

TEST_CASE("uart_register_event_callbacks argument checks", "[uart][hp-uart-only]")
{
    uart_event_callbacks_t cbs = {
        .on_rx_data = test_on_rx_data,
    };

    // Driver not installed yet
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, NULL));

    test_cb_state_init();
    test_cb_uart_setup(TEST_CB_RX_BUF, 0, 0, NULL);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, uart_register_event_callbacks(TEST_CB_UART_NUM, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, uart_register_event_callbacks(UART_NUM_MAX, &cbs, NULL));

    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));

    // An all NULL structure deregisters
    uart_event_callbacks_t empty_cbs = {};
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &empty_cbs, NULL));

    const char *msg = "deregistered";
    uart_write_bytes(TEST_CB_UART_NUM, msg, strlen(msg));
    TEST_ESP_OK(uart_wait_tx_done(TEST_CB_UART_NUM, pdMS_TO_TICKS(1000)));
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL(0, s_state.rx_data_calls);

    test_cb_uart_teardown();
    test_cb_state_deinit();
}

#if CONFIG_UART_ISR_IN_IRAM
// Deliberately left in flash, so that registering it has to be rejected
static bool test_on_rx_data_in_flash(uart_port_t uart_num, const uart_rx_data_event_data_t *edata, void *user_ctx)
{
    (void)uart_num;
    (void)edata;
    (void)user_ctx;
    return false;
}

TEST_CASE("uart_register_event_callbacks rejects callbacks outside IRAM", "[uart][hp-uart-only]")
{
    test_cb_state_init();
    test_cb_uart_setup(TEST_CB_RX_BUF, 0, 0, NULL);

    uart_event_callbacks_t bad_cbs = {
        .on_rx_data = test_on_rx_data_in_flash,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, uart_register_event_callbacks(TEST_CB_UART_NUM, &bad_cbs, NULL));

    uart_event_callbacks_t good_cbs = {
        .on_rx_data = test_on_rx_data,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &good_cbs, &s_state));

    test_cb_uart_teardown();
    test_cb_state_deinit();
}
#endif // CONFIG_UART_ISR_IN_IRAM

TEST_CASE("uart rx data callback fires without an event queue", "[uart][hp-uart-only]")
{
    const char *msg = "the quick brown fox jumps over the lazy dog";
    const size_t msg_len = strlen(msg);
    uint8_t rd_data[64] = {};

    test_cb_state_init();
    // Installed with queue_size 0 and uart_queue NULL, the whole point of the callback API
    test_cb_uart_setup(TEST_CB_RX_BUF, 0, 0, NULL);

    uart_event_callbacks_t cbs = {
        .on_rx_data = test_on_rx_data,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));

    TEST_ASSERT_EQUAL(msg_len, uart_write_bytes(TEST_CB_UART_NUM, msg, msg_len));
    TEST_ESP_OK(uart_wait_tx_done(TEST_CB_UART_NUM, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_state.rx_data_sem, pdMS_TO_TICKS(1000)));
    // Give the tail of the message time to arrive, it may be split over several interrupts
    vTaskDelay(pdMS_TO_TICKS(100));

    TEST_ASSERT_GREATER_THAN(0, s_state.rx_data_calls);
    TEST_ASSERT_EQUAL(msg_len, s_state.rx_bytes);
    TEST_ASSERT_TRUE(s_state.rx_timeout_seen);
    TEST_ASSERT_EQUAL(0, s_state.wrong_port_calls);
    TEST_ASSERT_EQUAL(0, s_state.wrong_ctx_calls);

    // The callback only signals availability, the data is still read from task context
    TEST_ASSERT_EQUAL(msg_len, uart_read_bytes(TEST_CB_UART_NUM, rd_data, msg_len, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(msg, rd_data, msg_len);

    test_cb_uart_teardown();
    test_cb_state_deinit();
}

TEST_CASE("uart pattern callback fires without an event queue", "[uart][hp-uart-only]")
{
    const char *msg = "AT+CWMODE?\r\n+++";

    test_cb_state_init();
    test_cb_uart_setup(TEST_CB_RX_BUF, 0, 0, NULL);

    uart_event_callbacks_t cbs = {
        .on_rx_data = test_on_rx_data,
        .on_pattern = test_on_pattern,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));
    TEST_ESP_OK(uart_enable_pattern_det_baud_intr(TEST_CB_UART_NUM, '+', 3, 9, 0, 0));
    TEST_ESP_OK(uart_pattern_queue_reset(TEST_CB_UART_NUM, 10));

    uart_write_bytes(TEST_CB_UART_NUM, msg, strlen(msg));
    TEST_ESP_OK(uart_wait_tx_done(TEST_CB_UART_NUM, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_state.pattern_sem, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_GREATER_THAN(0, s_state.pattern_calls);

    // The position still comes out of the pattern queue, from task context
    TEST_ASSERT_GREATER_OR_EQUAL(0, uart_pattern_pop_pos(TEST_CB_UART_NUM));

    TEST_ESP_OK(uart_disable_pattern_det_intr(TEST_CB_UART_NUM));
    test_cb_uart_teardown();
    test_cb_state_deinit();
}

TEST_CASE("uart rx error callback reports a full ring buffer", "[uart][hp-uart-only]")
{
    const int rx_buf_size = UART_HW_FIFO_LEN(TEST_CB_UART_NUM) + 16;
    uint8_t wr_data[128] = {};

    test_cb_state_init();
    test_cb_uart_setup(rx_buf_size, TEST_CB_TX_BUF, 0, NULL);

    uart_event_callbacks_t cbs = {
        .on_rx_error = test_on_rx_error,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));

    // Never read, so the RX ring buffer has to overflow
    for (int i = 0; i < (int)sizeof(wr_data); i++) {
        wr_data[i] = (uint8_t)i;
    }
    for (int i = 0; i < 8; i++) {
        uart_write_bytes(TEST_CB_UART_NUM, wr_data, sizeof(wr_data));
    }
    TEST_ESP_OK(uart_wait_tx_done(TEST_CB_UART_NUM, pdMS_TO_TICKS(2000)));

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_state.rx_error_sem, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_GREATER_THAN(0, s_state.rx_error_calls);
    TEST_ASSERT_EQUAL(UART_BUFFER_FULL, s_state.last_rx_error);

    TEST_ESP_OK(uart_flush_input(TEST_CB_UART_NUM));
    test_cb_uart_teardown();
    test_cb_state_deinit();
}

// Regression: the pattern detect event raised while the RX ring buffer is full used to be pushed to the
// event queue without checking that a queue exists, which asserted inside xQueueSendFromISR() for a driver
// installed with queue_size 0. That is exactly how the callback API is meant to be used.
TEST_CASE("uart pattern detection with a full ring buffer and no event queue", "[uart][hp-uart-only]")
{
    const int rx_buf_size = UART_HW_FIFO_LEN(TEST_CB_UART_NUM) + 16;
    uint8_t wr_data[128];

    test_cb_state_init();
    test_cb_uart_setup(rx_buf_size, TEST_CB_TX_BUF, 0, NULL);

    uart_event_callbacks_t cbs = {
        .on_pattern = test_on_pattern,
        .on_rx_error = test_on_rx_error,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));
    TEST_ESP_OK(uart_enable_pattern_det_baud_intr(TEST_CB_UART_NUM, '+', 3, 9, 0, 0));
    TEST_ESP_OK(uart_pattern_queue_reset(TEST_CB_UART_NUM, 10));

    // A stream that is nothing but the pattern, so pattern detection keeps firing while the ring buffer fills
    memset(wr_data, '+', sizeof(wr_data));
    for (int i = 0; i < 8; i++) {
        uart_write_bytes(TEST_CB_UART_NUM, wr_data, sizeof(wr_data));
    }
    TEST_ESP_OK(uart_wait_tx_done(TEST_CB_UART_NUM, pdMS_TO_TICKS(2000)));
    vTaskDelay(pdMS_TO_TICKS(200));

    // Surviving this far is the assertion: on the unpatched driver the ISR aborts inside xQueueSendFromISR
    TEST_ASSERT_GREATER_THAN(0, s_state.pattern_calls);

    TEST_ESP_OK(uart_disable_pattern_det_intr(TEST_CB_UART_NUM));
    TEST_ESP_OK(uart_flush_input(TEST_CB_UART_NUM));
    test_cb_uart_teardown();
    test_cb_state_deinit();
}

TEST_CASE("uart tx done callback fires with a tx ring buffer", "[uart][hp-uart-only]")
{
    uint8_t wr_data[512];

    test_cb_state_init();
    test_cb_uart_setup(TEST_CB_RX_BUF, TEST_CB_TX_BUF, 0, NULL);

    uart_event_callbacks_t cbs = {
        .on_tx_done = test_on_tx_done,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));

    memset(wr_data, 0x5a, sizeof(wr_data));
    TEST_ASSERT_EQUAL(sizeof(wr_data), uart_write_bytes(TEST_CB_UART_NUM, wr_data, sizeof(wr_data)));

    // No uart_wait_tx_done() here, the tx_idle event has to arrive on its own
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_state.tx_idle_sem, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_GREATER_THAN(0, s_state.tx_space_calls);
    TEST_ASSERT_GREATER_THAN(0, s_state.tx_idle_calls);

    TEST_ESP_OK(uart_flush_input(TEST_CB_UART_NUM));
    test_cb_uart_teardown();
    test_cb_state_deinit();
}

TEST_CASE("uart tx done callback fires without a tx ring buffer", "[uart][hp-uart-only]")
{
    uint8_t wr_data[512];

    test_cb_state_init();
    test_cb_uart_setup(TEST_CB_RX_BUF, 0, 0, NULL);

    uart_event_callbacks_t cbs = {
        .on_tx_done = test_on_tx_done,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));

    memset(wr_data, 0xa5, sizeof(wr_data));
    TEST_ASSERT_EQUAL(sizeof(wr_data), uart_write_bytes(TEST_CB_UART_NUM, wr_data, sizeof(wr_data)));

    // No uart_wait_tx_done() here either
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_state.tx_idle_sem, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_GREATER_THAN(0, s_state.tx_idle_calls);
    // Without a TX ring buffer there is no chunk to return, so no "space available" events
    TEST_ASSERT_EQUAL(0, s_state.tx_space_calls);

    TEST_ESP_OK(uart_flush_input(TEST_CB_UART_NUM));
    test_cb_uart_teardown();
    test_cb_state_deinit();
}

// The tx_idle event needs UART_INTR_TX_DONE, which uart_wait_tx_done() also drives. Make sure the two do
// not trip over each other.
TEST_CASE("uart_wait_tx_done works with a registered tx done callback", "[uart][hp-uart-only]")
{
    uint8_t wr_data[256];

    test_cb_state_init();
    test_cb_uart_setup(TEST_CB_RX_BUF, TEST_CB_TX_BUF, 0, NULL);

    uart_event_callbacks_t cbs = {
        .on_tx_done = test_on_tx_done,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));

    memset(wr_data, 0x3c, sizeof(wr_data));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL(sizeof(wr_data), uart_write_bytes(TEST_CB_UART_NUM, wr_data, sizeof(wr_data)));
        TEST_ESP_OK(uart_wait_tx_done(TEST_CB_UART_NUM, pdMS_TO_TICKS(2000)));
    }
    TEST_ASSERT_GREATER_THAN(0, s_state.tx_idle_calls);

    TEST_ESP_OK(uart_flush_input(TEST_CB_UART_NUM));
    test_cb_uart_teardown();
    test_cb_state_deinit();
}

TEST_CASE("uart event queue and callbacks can be used together", "[uart][hp-uart-only]")
{
    const char *msg = "queue and callbacks";
    QueueHandle_t queue = NULL;
    uart_event_t event = {};
    bool got_data_event = false;

    test_cb_state_init();
    test_cb_uart_setup(TEST_CB_RX_BUF, 0, 20, &queue);
    TEST_ASSERT_NOT_NULL(queue);

    uart_event_callbacks_t cbs = {
        .on_rx_data = test_on_rx_data,
    };
    TEST_ESP_OK(uart_register_event_callbacks(TEST_CB_UART_NUM, &cbs, &s_state));

    uart_write_bytes(TEST_CB_UART_NUM, msg, strlen(msg));
    TEST_ESP_OK(uart_wait_tx_done(TEST_CB_UART_NUM, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_state.rx_data_sem, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_GREATER_THAN(0, s_state.rx_data_calls);

    while (xQueueReceive(queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (event.type == UART_DATA) {
            got_data_event = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(got_data_event);

    TEST_ESP_OK(uart_flush_input(TEST_CB_UART_NUM));
    test_cb_uart_teardown();
    test_cb_state_deinit();
}
