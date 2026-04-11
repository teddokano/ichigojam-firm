// IchigoJam_Z - keyboard/UART HAL for Zephyr

#ifndef __KEYBOARD_H__
#define __KEYBOARD_H__

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

// UART device from chosen node (uart0 via overlay)
#define UART_DEV DT_CHOSEN(zephyr_console)

static const struct device *uart_dev;

// Message queue: 64 bytes, 1-byte items
K_MSGQ_DEFINE(uart_msgq, 1, 64, 1);

static void uart_rx_cb(const struct device *dev, void *user_data)
{
    uint8_t ch;

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        if (uart_fifo_read(dev, &ch, 1) == 1) {
            if (ch == '\r') {
                ch = '\n'; // CR -> LF
            }
            if (ch == 27) { // ESC
                _g.key_flg_esc = (_g.uartmode_rxd & 2) == 0;
            }
            k_msgq_put(&uart_msgq, &ch, K_NO_WAIT);
        }
    }
}

S_INLINE uint key_getKeyboardID(void) {
    return 0;
}

struct keyflg_def key_flg;
uint8 displaymode;

// keybuf is referenced by key_clearKey()
char* keybuf = (char*)RAM_KEYBUF + 1;

S_INLINE void key_init(void) {
}

S_INLINE void key_send_reset(void) {
}

S_INLINE void key_enable(uint8 b) {
}

INLINE int key_btn(int n) {
    return 0;
}

int key_getKey(void) {
    char ch;
    if (k_msgq_get(&uart_msgq, &ch, K_NO_WAIT) == 0) {
        return (int)(uint8_t)ch;
    }
    k_sleep(K_MSEC(1));
    return -1;
}

void key_clearKey(void) {
    k_msgq_purge(&uart_msgq);
    *keybuf = 0;
}

void uart_init(void) {
    uart_dev = DEVICE_DT_GET(UART_DEV);
    if (!device_is_ready(uart_dev)) {
        return;
    }
    uart_irq_callback_user_data_set(uart_dev, uart_rx_cb, NULL);
    uart_irq_rx_enable(uart_dev);
    _g.uartmode_txd = 1;
}

INLINE void IJB_uart(int16 txd, int16 rxd) {
}

void uart_putc(char c) {
    if (uart_dev && device_is_ready(uart_dev)) {
        uart_poll_out(uart_dev, c);
    }
}

void uart_bps(int n) {
}

void put_chr(char c) {
    if (_g.uartmode_txd > 0) {
        uart_putc(c);
    }
    screen_putc(c);
}

INLINE int stopExecute(void) {
    return _g.key_flg_esc;
}

#endif // __KEYBOARD_H__
