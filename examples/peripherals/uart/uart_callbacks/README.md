| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 | ESP32-S31 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- | --------- |

# UART Event Callbacks Example

(See the README.md file in the upper level 'examples' directory for more information about examples.)

This example shows how to receive UART events as ISR callbacks instead of through the driver's FreeRTOS
event queue, using `uart_register_event_callbacks()`.

The driver is installed with `uart_queue` set to NULL, so no event queue is created and no task has to sit
blocked on one. The callbacks run in interrupt context and do nothing but post a small fixed-size record
onto the application's own event loop queue; `app_main()` is the only task that touches the port, and the
received bytes are never copied out of the driver's RX ring buffer before the loop reads them.

Compare with the [uart_events](../uart_events) example, which does the same job with a dedicated 4096-byte
event task.

The example reads data from `UART0`, echoes it back to the monitoring console, and detects `+++` in the
incoming stream using the driver's pattern detection.

## How to use example

### Hardware Required

The example can be run on any development board, that is based on the Espressif SoC. The board shall be
connected to a computer with a single USB cable for flashing and monitoring.

### Configure the project

```
idf.py menuconfig
```

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Example Output

Type something into the monitor console and it is echoed back, with the UART data event reported by the
loop:

```
I (325) uart_callbacks: waiting for UART events on a single application loop, no UART task
I (5325) uart_callbacks: [UART DATA]: 5
I (9135) uart_callbacks: [UART PATTERN DETECTED] pos: 0
I (9135) uart_callbacks: read data: , read pattern: +++
```

## Notes

* The callbacks run in ISR context. They must not block, and may only call FreeRTOS APIs with the `FromISR`
  suffix.
* `on_rx_data` reports that data is available, it does not deliver it. `uart_read_bytes()` takes a mutex and
  must still be called from task context, and so must `uart_pattern_pop_pos()`.
* If `CONFIG_UART_ISR_IN_IRAM` is enabled, the callbacks, everything they call, and the user context must be
  in internal RAM. This example marks its callbacks `IRAM_ATTR` so that it works either way.

## Troubleshooting

For any technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. We
will get back to you soon.
