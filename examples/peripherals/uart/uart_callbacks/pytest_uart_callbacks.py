# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@idf_parametrize('target', ['supported_targets'], indirect=['target'])
def test_uart_callbacks_example(dut: Dut) -> None:
    dut.expect_exact('waiting for UART events on a single application loop, no UART task')
    dut.write('a')
    dut.expect_exact('uart_callbacks: [UART DATA]: 2')  # dut.write will add an extra '\n'
    dut.write('HA')
    dut.expect_exact('uart_callbacks: [UART DATA]: 3')  # dut.write will add an extra '\n'
    dut.write('+++')
    dut.expect_exact('uart_callbacks: [UART PATTERN DETECTED]')
